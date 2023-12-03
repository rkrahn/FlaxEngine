// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

#include "ImportModel.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Core/Log.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Core/Collections/ArrayExtensions.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Graphics/Models/ModelData.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Content.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/RectPack.h"
#include "AssetsImportingManager.h"

bool ImportModel::TryGetImportOptions(const StringView& path, Options& options)
{
    if (FileSystem::FileExists(path))
    {
        // Try to load asset file and asset info
        auto tmpFile = ContentStorageManager::GetStorage(path);
        AssetInitData data;
        if (tmpFile
            && tmpFile->GetEntriesCount() == 1
            && (
                (tmpFile->GetEntry(0).TypeName == Model::TypeName && !tmpFile->LoadAssetHeader(0, data) && data.SerializedVersion >= 4)
                ||
                (tmpFile->GetEntry(0).TypeName == SkinnedModel::TypeName && !tmpFile->LoadAssetHeader(0, data) && data.SerializedVersion >= 1)
                ||
                (tmpFile->GetEntry(0).TypeName == Animation::TypeName && !tmpFile->LoadAssetHeader(0, data) && data.SerializedVersion >= 1)
            ))
        {
            // Check import meta
            rapidjson_flax::Document metadata;
            metadata.Parse((const char*)data.Metadata.Get(), data.Metadata.Length());
            if (metadata.HasParseError() == false)
            {
                options.Deserialize(metadata, nullptr);
                return true;
            }
        }
    }
    return false;
}

void RepackMeshLightmapUVs(ModelData& data)
{
    // Use weight-based coordinates space placement and rect-pack to allocate more space for bigger meshes in the model lightmap chart
    int32 lodIndex = 0;
    auto& lod = data.LODs[lodIndex];

    // Build list of meshes with their area
    struct LightmapUVsPack : RectPack<LightmapUVsPack, float>
    {
        LightmapUVsPack(float x, float y, float width, float height)
            : RectPack<LightmapUVsPack, float>(x, y, width, height)
        {
        }

        void OnInsert()
        {
        }
    };
    struct MeshEntry
    {
        MeshData* Mesh;
        float Area;
        float Size;
        LightmapUVsPack* Slot;
    };
    Array<MeshEntry> entries;
    entries.Resize(lod.Meshes.Count());
    float areaSum = 0;
    for (int32 meshIndex = 0; meshIndex < lod.Meshes.Count(); meshIndex++)
    {
        auto& entry = entries[meshIndex];
        entry.Mesh = lod.Meshes[meshIndex];
        entry.Area = entry.Mesh->CalculateTrianglesArea();
        entry.Size = Math::Sqrt(entry.Area);
        areaSum += entry.Area;
    }

    if (areaSum > ZeroTolerance)
    {
        // Pack all surfaces into atlas
        float atlasSize = Math::Sqrt(areaSum) * 1.02f;
        int32 triesLeft = 10;
        while (triesLeft--)
        {
            bool failed = false;
            const float chartsPadding = (4.0f / 256.0f) * atlasSize;
            LightmapUVsPack root(chartsPadding, chartsPadding, atlasSize - chartsPadding, atlasSize - chartsPadding);
            for (auto& entry : entries)
            {
                entry.Slot = root.Insert(entry.Size, entry.Size, chartsPadding);
                if (entry.Slot == nullptr)
                {
                    // Failed to insert surface, increase atlas size and try again
                    atlasSize *= 1.5f;
                    failed = true;
                    break;
                }
            }

            if (!failed)
            {
                // Transform meshes lightmap UVs into the slots in the whole atlas
                const float atlasSizeInv = 1.0f / atlasSize;
                for (const auto& entry : entries)
                {
                    Float2 uvOffset(entry.Slot->X * atlasSizeInv, entry.Slot->Y * atlasSizeInv);
                    Float2 uvScale((entry.Slot->Width - chartsPadding) * atlasSizeInv, (entry.Slot->Height - chartsPadding) * atlasSizeInv);
                    // TODO: SIMD
                    for (auto& uv : entry.Mesh->LightmapUVs)
                    {
                        uv = uv * uvScale + uvOffset;
                    }
                }
                break;
            }
        }
    }
}

void TryRestoreMaterials(CreateAssetContext& context, ModelData& modelData)
{
    // Skip if file is missing
    if (!FileSystem::FileExists(context.TargetAssetPath))
        return;

    // Try to load asset that gets reimported
    AssetReference<Asset> asset = Content::LoadAsync<Asset>(context.TargetAssetPath);
    if (asset == nullptr)
        return;
    if (asset->WaitForLoaded())
        return;

    // Get model object
    ModelBase* model = nullptr;
    if (asset.Get()->GetTypeName() == Model::TypeName)
    {
        model = ((Model*)asset.Get());
    }
    else if (asset.Get()->GetTypeName() == SkinnedModel::TypeName)
    {
        model = ((SkinnedModel*)asset.Get());
    }
    if (!model)
        return;

    // Peek materials
    for (int32 i = 0; i < modelData.Materials.Count(); i++)
    {
        auto& dstSlot = modelData.Materials[i];

        if (model->MaterialSlots.Count() > i)
        {
            auto& srcSlot = model->MaterialSlots[i];

            dstSlot.Name = srcSlot.Name;
            dstSlot.ShadowsMode = srcSlot.ShadowsMode;
            dstSlot.AssetID = srcSlot.Material.GetID();
        }
    }
}

void SetupMaterialSlots(ModelData& data, const Array<MaterialSlotEntry>& materials)
{
    Array<int32> materialSlotsTable;
    materialSlotsTable.Resize(materials.Count());
    materialSlotsTable.SetAll(-1);
    for (auto& lod : data.LODs)
    {
        for (MeshData* mesh : lod.Meshes)
        {
            int32 newSlotIndex = materialSlotsTable[mesh->MaterialSlotIndex];
            if (newSlotIndex == -1)
            {
                newSlotIndex = data.Materials.Count();
                data.Materials.AddOne() = materials[mesh->MaterialSlotIndex];
            }
            mesh->MaterialSlotIndex = newSlotIndex;
        }
    }
}

bool SortMeshGroups(IGrouping<StringView, MeshData*> const& i1, IGrouping<StringView, MeshData*> const& i2)
{
    return i1.GetKey().Compare(i2.GetKey()) < 0;
}

CreateAssetResult ImportModel::Import(CreateAssetContext& context)
{
    // Get import options
    Options options;
    if (context.CustomArg != nullptr)
    {
        // Copy import options from argument
        options = *static_cast<Options*>(context.CustomArg);
    }
    else
    {
        // Restore the previous settings or use default ones
        if (!TryGetImportOptions(context.TargetAssetPath, options))
        {
            LOG(Warning, "Missing model import options. Using default values.");
        }
    }

    // Import model file
    ModelData* data = options.Cached ? options.Cached->Data : nullptr;
    ModelData dataThis;
    Array<IGrouping<StringView, MeshData*>>* meshesByNamePtr = options.Cached ? (Array<IGrouping<StringView, MeshData*>>*)options.Cached->MeshesByName : nullptr;
    Array<IGrouping<StringView, MeshData*>> meshesByNameThis;
    if (!data)
    {
        String errorMsg;
        String autoImportOutput(StringUtils::GetDirectoryName(context.TargetAssetPath));
        autoImportOutput /= options.SubAssetFolder.HasChars() ? options.SubAssetFolder.TrimTrailing() : String(StringUtils::GetFileNameWithoutExtension(context.InputPath));
        if (ModelTool::ImportModel(context.InputPath, dataThis, options, errorMsg, autoImportOutput))
        {
            LOG(Error, "Cannot import model file. {0}", errorMsg);
            return CreateAssetResult::Error;
        }
        data = &dataThis;

        // Group meshes by the name (the same mesh name can be used by multiple meshes that use different materials)
        if (data->LODs.Count() != 0)
        {
            const Function<StringView(MeshData* const&)> f = [](MeshData* const& x) -> StringView
            {
                return x->Name;
            };
            ArrayExtensions::GroupBy(data->LODs[0].Meshes, f, meshesByNameThis);
            Sorting::QuickSort(meshesByNameThis.Get(), meshesByNameThis.Count(), &SortMeshGroups);
        }
        meshesByNamePtr = &meshesByNameThis;
    }
    Array<IGrouping<StringView, MeshData*>>& meshesByName = *meshesByNamePtr;

    // Import objects from file separately
    if (options.SplitObjects)
    {
        // Import the first object within this call
        options.SplitObjects = false;
        options.ObjectIndex = 0;

        // Import rest of the objects recursive but use current model data to skip loading file again
        ModelTool::Options::CachedData cached = { data, (void*)meshesByNamePtr };
        options.Cached = &cached;
        Function<bool(Options& splitOptions, const StringView& objectName)> splitImport;
        splitImport.Bind([&context](Options& splitOptions, const StringView& objectName)
        {
            // Recursive importing of the split object
            String postFix = objectName;
            const int32 splitPos = postFix.FindLast(TEXT('|'));
            if (splitPos != -1)
                postFix = postFix.Substring(splitPos + 1);
            const String outputPath = String(StringUtils::GetPathWithoutExtension(context.TargetAssetPath)) + TEXT(" ") + postFix + TEXT(".flax");
            return AssetsImportingManager::Import(context.InputPath, outputPath, &splitOptions);
        });
        auto splitOptions = options;
        switch (options.Type)
        {
        case ModelTool::ModelType::Model:
        case ModelTool::ModelType::SkinnedModel:
            LOG(Info, "Splitting imported {0} meshes", meshesByName.Count());
            for (int32 groupIndex = 1; groupIndex < meshesByName.Count(); groupIndex++)
            {
                auto& group = meshesByName[groupIndex];
                splitOptions.ObjectIndex = groupIndex;
                splitImport(splitOptions, group.GetKey());
            }
            break;
        case ModelTool::ModelType::Animation:
            LOG(Info, "Splitting imported {0} animations", data->Animations.Count());
            for (int32 i = 1; i < data->Animations.Count(); i++)
            {
                auto& animation = data->Animations[i];
                splitOptions.ObjectIndex = i;
                splitImport(splitOptions, animation.Name);
            }
            break;
        }
    }

    // When importing a single object as model asset then select a specific mesh group
    Array<MeshData*> meshesToDelete;
    if (options.ObjectIndex >= 0 &&
        options.ObjectIndex < meshesByName.Count() &&
        (options.Type == ModelTool::ModelType::Model || options.Type == ModelTool::ModelType::SkinnedModel))
    {
        auto& group = meshesByName[options.ObjectIndex];
        if (&dataThis == data)
        {
            // Use meshes only from the the grouping (others will be removed manually)
            {
                auto& lod = dataThis.LODs[0];
                meshesToDelete.Add(lod.Meshes);
                lod.Meshes.Clear();
                for (MeshData* mesh : group)
                {
                    lod.Meshes.Add(mesh);
                    meshesToDelete.Remove(mesh);
                }
            }
            for (int32 lodIndex = 1; lodIndex < dataThis.LODs.Count(); lodIndex++)
            {
                auto& lod = dataThis.LODs[lodIndex];
                Array<MeshData*> lodMeshes = lod.Meshes;
                lod.Meshes.Clear();
                for (MeshData* lodMesh : lodMeshes)
                {
                    if (lodMesh->Name == group.GetKey())
                        lod.Meshes.Add(lodMesh);
                    else
                        meshesToDelete.Add(lodMesh);
                }
            }

            // Use only materials references by meshes from the first grouping
            {
                auto materials = dataThis.Materials;
                dataThis.Materials.Clear();
                SetupMaterialSlots(dataThis, materials);
            }
        }
        else
        {
            // Copy data from others data
            dataThis.Skeleton = data->Skeleton;
            dataThis.Nodes = data->Nodes;

            // Move meshes from this group (including any LODs of them)
            {
                auto& lod = dataThis.LODs.AddOne();
                lod.ScreenSize = data->LODs[0].ScreenSize;
                lod.Meshes.Add(group);
                for (MeshData* mesh : group)
                    data->LODs[0].Meshes.Remove(mesh);
            }
            for (int32 lodIndex = 1; lodIndex < data->LODs.Count(); lodIndex++)
            {
                Array<MeshData*> lodMeshes = data->LODs[lodIndex].Meshes;
                for (int32 i = lodMeshes.Count() - 1; i >= 0; i--)
                {
                    MeshData* lodMesh = lodMeshes[i];
                    if (lodMesh->Name == group.GetKey())
                        data->LODs[lodIndex].Meshes.Remove(lodMesh);
                    else
                        lodMeshes.RemoveAtKeepOrder(i);
                }
                if (lodMeshes.Count() == 0)
                    break; // No meshes of that name in this LOD so skip further ones
                auto& lod = dataThis.LODs.AddOne();
                lod.ScreenSize = data->LODs[lodIndex].ScreenSize;
                lod.Meshes.Add(lodMeshes);
            }

            // Copy materials used by the meshes
            SetupMaterialSlots(dataThis, data->Materials);
        }
        data = &dataThis;
    }

    // Check if restore materials on model reimport
    if (options.RestoreMaterialsOnReimport && data->Materials.HasItems())
    {
        TryRestoreMaterials(context, *data);
    }

    // When using generated lightmap UVs those coordinates needs to be moved so all meshes are in unique locations in [0-1]x[0-1] coordinates space
    // Model importer generates UVs in [0-1] space for each mesh so now we need to pack them inside the whole model (only when using multiple meshes)
    if (options.Type == ModelTool::ModelType::Model && options.LightmapUVsSource == ModelLightmapUVsSource::Generate && data->LODs.HasItems() && data->LODs[0].Meshes.Count() > 1)
    {
        RepackMeshLightmapUVs(*data);
    }

    // Create destination asset type
    CreateAssetResult result = CreateAssetResult::InvalidTypeID;
    switch (options.Type)
    {
    case ModelTool::ModelType::Model:
        result = CreateModel(context, *data, &options);
        break;
    case ModelTool::ModelType::SkinnedModel:
        result = CreateSkinnedModel(context, *data, &options);
        break;
    case ModelTool::ModelType::Animation:
        result = CreateAnimation(context, *data, &options);
        break;
    }
    for (auto mesh : meshesToDelete)
        Delete(mesh);
    if (result != CreateAssetResult::Ok)
        return result;

    // Create json with import context
    rapidjson_flax::StringBuffer importOptionsMetaBuffer;
    importOptionsMetaBuffer.Reserve(256);
    CompactJsonWriter importOptionsMetaObj(importOptionsMetaBuffer);
    JsonWriter& importOptionsMeta = importOptionsMetaObj;
    importOptionsMeta.StartObject();
    {
        context.AddMeta(importOptionsMeta);
        options.Serialize(importOptionsMeta, nullptr);
    }
    importOptionsMeta.EndObject();
    context.Data.Metadata.Copy((const byte*)importOptionsMetaBuffer.GetString(), (uint32)importOptionsMetaBuffer.GetSize());

    return CreateAssetResult::Ok;
}

CreateAssetResult ImportModel::Create(CreateAssetContext& context)
{
    ASSERT(context.CustomArg != nullptr);
    auto& modelData = *(ModelData*)context.CustomArg;

    // Ensure model has any meshes
    if ((modelData.LODs.IsEmpty() || modelData.LODs[0].Meshes.IsEmpty()))
    {
        LOG(Warning, "Models has no valid meshes");
        return CreateAssetResult::Error;
    }

    // Auto calculate LODs transition settings
    modelData.CalculateLODsScreenSizes();

    return CreateModel(context, modelData);
}

CreateAssetResult ImportModel::CreateModel(CreateAssetContext& context, ModelData& modelData, const Options* options)
{
    IMPORT_SETUP(Model, Model::SerializedVersion);

    // Save model header
    MemoryWriteStream stream(4096);
    if (modelData.Pack2ModelHeader(&stream))
        return CreateAssetResult::Error;
    if (context.AllocateChunk(0))
        return CreateAssetResult::CannotAllocateChunk;
    context.Data.Header.Chunks[0]->Data.Copy(stream.GetHandle(), stream.GetPosition());

    // Pack model LODs data
    const auto lodCount = modelData.GetLODsCount();
    for (int32 lodIndex = 0; lodIndex < lodCount; lodIndex++)
    {
        stream.SetPosition(0);

        // Pack meshes
        auto& meshes = modelData.LODs[lodIndex].Meshes;
        for (int32 meshIndex = 0; meshIndex < meshes.Count(); meshIndex++)
        {
            if (meshes[meshIndex]->Pack2Model(&stream))
            {
                LOG(Warning, "Cannot pack mesh.");
                return CreateAssetResult::Error;
            }
        }

        const int32 chunkIndex = lodIndex + 1;
        if (context.AllocateChunk(chunkIndex))
            return CreateAssetResult::CannotAllocateChunk;
        context.Data.Header.Chunks[chunkIndex]->Data.Copy(stream.GetHandle(), stream.GetPosition());
    }

    // Generate SDF
    if (options && options->GenerateSDF)
    {
        stream.SetPosition(0);
        if (!ModelTool::GenerateModelSDF(nullptr, &modelData, options->SDFResolution, lodCount - 1, nullptr, &stream, context.TargetAssetPath))
        {
            if (context.AllocateChunk(15))
                return CreateAssetResult::CannotAllocateChunk;
            context.Data.Header.Chunks[15]->Data.Copy(stream.GetHandle(), stream.GetPosition());
        }
    }

    return CreateAssetResult::Ok;
}

CreateAssetResult ImportModel::CreateSkinnedModel(CreateAssetContext& context, ModelData& modelData, const Options* options)
{
    IMPORT_SETUP(SkinnedModel, SkinnedModel::SerializedVersion);

    // Save skinned model header
    MemoryWriteStream stream(4096);
    if (modelData.Pack2SkinnedModelHeader(&stream))
        return CreateAssetResult::Error;
    if (context.AllocateChunk(0))
        return CreateAssetResult::CannotAllocateChunk;
    context.Data.Header.Chunks[0]->Data.Copy(stream.GetHandle(), stream.GetPosition());

    // Pack model LODs data
    const auto lodCount = modelData.GetLODsCount();
    for (int32 lodIndex = 0; lodIndex < lodCount; lodIndex++)
    {
        stream.SetPosition(0);

        // Mesh Data Version
        stream.WriteByte(1);

        // Pack meshes
        auto& meshes = modelData.LODs[lodIndex].Meshes;
        for (int32 meshIndex = 0; meshIndex < meshes.Count(); meshIndex++)
        {
            if (meshes[meshIndex]->Pack2SkinnedModel(&stream))
            {
                LOG(Warning, "Cannot pack mesh.");
                return CreateAssetResult::Error;
            }
        }

        const int32 chunkIndex = lodIndex + 1;
        if (context.AllocateChunk(chunkIndex))
            return CreateAssetResult::CannotAllocateChunk;
        context.Data.Header.Chunks[chunkIndex]->Data.Copy(stream.GetHandle(), stream.GetPosition());
    }

    return CreateAssetResult::Ok;
}

CreateAssetResult ImportModel::CreateAnimation(CreateAssetContext& context, ModelData& modelData, const Options* options)
{
    IMPORT_SETUP(Animation, Animation::SerializedVersion);

    // Save animation data
    MemoryWriteStream stream(8182);
    const int32 animIndex = options && options->ObjectIndex != -1 ? options->ObjectIndex : 0; // Single animation per asset
    if (modelData.Pack2AnimationHeader(&stream, animIndex))
        return CreateAssetResult::Error;
    if (context.AllocateChunk(0))
        return CreateAssetResult::CannotAllocateChunk;
    context.Data.Header.Chunks[0]->Data.Copy(stream.GetHandle(), stream.GetPosition());

    return CreateAssetResult::Ok;
}

#endif