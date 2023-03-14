/*
 *  Copyright 2019-2022 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include <vector>
#include <memory>
#include <cmath>
#include <limits>

#include "GLTFLoader.hpp"
#include "MapHelper.hpp"
#include "CommonlyUsedStates.h"
#include "DataBlobImpl.hpp"
#include "Image.h"
#include "FileSystem.hpp"
#include "FileWrapper.hpp"
#include "GraphicsAccessories.hpp"
#include "TextureLoader.h"
#include "GraphicsUtilities.h"
#include "Align.hpp"
#include "GLTFBuilder.hpp"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE

#if defined(_MSC_VER) && defined(TINYGLTF_ENABLE_DRACO)
#    pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#endif
#include "../../ThirdParty/tinygltf/tiny_gltf.h"

namespace Diligent
{

namespace GLTF
{

namespace
{

VALUE_TYPE TinyGltfComponentTypeToValueType(int GltfCompType)
{
    switch (GltfCompType)
    {
        // clang-format off
        case TINYGLTF_COMPONENT_TYPE_BYTE:           return VT_INT8;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return VT_UINT8;
        case TINYGLTF_COMPONENT_TYPE_SHORT:          return VT_INT16;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return VT_UINT16;
        case TINYGLTF_COMPONENT_TYPE_INT:            return VT_INT32;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return VT_UINT32;
        case TINYGLTF_COMPONENT_TYPE_FLOAT:          return VT_FLOAT32;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:         return VT_FLOAT64;
        // clang-format on
        default:
            UNEXPECTED("Unknown GLTF component type");
            return VT_UNDEFINED;
    }
}


struct TinyGltfNodeWrapper
{
    const tinygltf::Node& Node;

    // clang-format off
    const auto& GetName()        const { return Node.name; }
    const auto& GetSkin()        const { return Node.skin; }
    const auto& GetTranslation() const { return Node.translation; }
    const auto& GetRotation()    const { return Node.rotation; }
    const auto& GetScale()       const { return Node.scale; }
    const auto& GetMatrix()      const { return Node.matrix; }
    const auto& GetChildrenIds() const { return Node.children; }
    auto        GetMeshId()      const { return Node.mesh; }
    auto        GetCameraId()    const { return Node.camera; }
    // clang-format on
};

struct TinyGltfPrimitiveWrapper
{
    const tinygltf::Primitive& Primitive;

    const int* GetAttribute(const char* Name) const
    {
        auto attrib_it = Primitive.attributes.find(Name);
        return attrib_it != Primitive.attributes.end() ?
            &attrib_it->second :
            nullptr;
    }

    auto GetIndicesId() const { return Primitive.indices; }
    auto GetMaterialId() const { return Primitive.material; }
};

struct TinyGltfMeshWrapper
{
    const tinygltf::Mesh& Mesh;

    const auto& Get() const { return Mesh; }

    auto GetPrimitiveCount() const { return Mesh.primitives.size(); }
    auto GetPrimitive(size_t Idx) const { return TinyGltfPrimitiveWrapper{Mesh.primitives[Idx]}; };
};

struct TinyGltfBufferViewWrapper;
struct TinyGltfAccessorWrapper
{
    const tinygltf::Accessor& Accessor;

    auto GetCount() const { return Accessor.count; }
    auto GetMinValues() const
    {
        return float3{
            static_cast<float>(Accessor.minValues[0]),
            static_cast<float>(Accessor.minValues[1]),
            static_cast<float>(Accessor.minValues[2]),
        };
    }
    auto GetMaxValues() const
    {
        return float3{
            static_cast<float>(Accessor.maxValues[0]),
            static_cast<float>(Accessor.maxValues[1]),
            static_cast<float>(Accessor.maxValues[2]),
        };
    }

    // clang-format off
    auto GetBufferViewId()  const { return Accessor.bufferView; }
    auto GetByteOffset()    const { return Accessor.byteOffset; }
    auto GetComponentType() const { return TinyGltfComponentTypeToValueType(Accessor.componentType); }
    auto GetNumComponents() const { return tinygltf::GetNumComponentsInType(Accessor.type); }
    // clang-format on
    auto GetByteStride(const TinyGltfBufferViewWrapper& View) const;
};

struct TinyGltfPerspectiveCameraWrapper
{
    const tinygltf::PerspectiveCamera& Camera;

    // clang-format off
    auto GetAspectRatio() const { return Camera.aspectRatio; }
    auto GetYFov()        const { return Camera.yfov; }
    auto GetZNear()       const { return Camera.znear; }
    auto GetZFar()        const { return Camera.zfar; }
    // clang-format on
};

struct TinyGltfOrthoCameraWrapper
{
    const tinygltf::OrthographicCamera& Camera;

    // clang-format off
    auto GetXMag()  const { return Camera.xmag; }
    auto GetYMag()  const { return Camera.ymag; }
    auto GetZNear() const { return Camera.znear; }
    auto GetZFar()  const { return Camera.zfar; }
    // clang-format on
};

struct TinyGltfCameraWrapper
{
    const tinygltf::Camera& Camera;

    const auto& GetName() const { return Camera.name; }
    const auto& GetType() const { return Camera.type; }
    auto        GetPerspective() const { return TinyGltfPerspectiveCameraWrapper{Camera.perspective}; }
    auto        GetOrthographic() const { return TinyGltfOrthoCameraWrapper{Camera.orthographic}; }
};

struct TinyGltfBufferViewWrapper
{
    const tinygltf::BufferView& View;

    auto GetBufferId() const { return View.buffer; }
    auto GetByteOffset() const { return View.byteOffset; }
};

struct TinyGltfBufferWrapper
{
    const tinygltf::Buffer& Buffer;

    const auto* GetData(size_t Offset) const { return &Buffer.data[Offset]; }
};

struct TinyGltfSkinWrapper
{
    const tinygltf::Skin& Skin;

    const auto& GetName() const { return Skin.name; }
    auto        GetSkeletonId() const { return Skin.skeleton; }
    auto        GetInverseBindMatricesId() const { return Skin.inverseBindMatrices; }
    const auto& GetJointIds() const { return Skin.joints; }
};

struct TinyGltfAnimationSamplerWrapper
{
    const tinygltf::AnimationSampler& Sam;

    AnimationSampler::INTERPOLATION_TYPE GetInterpolation() const
    {
        if (Sam.interpolation == "LINEAR")
            return AnimationSampler::INTERPOLATION_TYPE::LINEAR;
        else if (Sam.interpolation == "STEP")
            return AnimationSampler::INTERPOLATION_TYPE::STEP;
        else if (Sam.interpolation == "CUBICSPLINE")
            return AnimationSampler::INTERPOLATION_TYPE::CUBICSPLINE;
        else
        {
            UNEXPECTED("Unexpected animation interpolation type: ", Sam.interpolation);
            return AnimationSampler::INTERPOLATION_TYPE::LINEAR;
        }
    }

    auto GetInputId() const { return Sam.input; }
    auto GetOutputId() const { return Sam.output; }
};


struct TinyGltfAnimationChannelWrapper
{
    const tinygltf::AnimationChannel& Channel;

    AnimationChannel::PATH_TYPE GetPathType() const
    {
        if (Channel.target_path == "rotation")
            return AnimationChannel::PATH_TYPE::ROTATION;
        else if (Channel.target_path == "translation")
            return AnimationChannel::PATH_TYPE::TRANSLATION;
        else if (Channel.target_path == "scale")
            return AnimationChannel::PATH_TYPE::SCALE;
        else if (Channel.target_path == "weights")
            return AnimationChannel::PATH_TYPE::WEIGHTS;
        else
        {
            UNEXPECTED("Unsupported animation channel path ", Channel.target_path);
            return AnimationChannel::PATH_TYPE::ROTATION;
        }
    }

    auto GetSamplerId() const { return Channel.sampler; }
    auto GetTargetNodeId() const { return Channel.target_node; }
};

struct TinyGltfAnimationWrapper
{
    const tinygltf::Animation& Anim;

    const auto& GetName() const { return Anim.name; }

    auto GetSamplerCount() const { return Anim.samplers.size(); }
    auto GetChannelCount() const { return Anim.channels.size(); }
    auto GetSampler(size_t Id) const { return TinyGltfAnimationSamplerWrapper{Anim.samplers[Id]}; }
    auto GetChannel(size_t Id) const { return TinyGltfAnimationChannelWrapper{Anim.channels[Id]}; }
};

struct TinyGltfModelWrapper
{
    const tinygltf::Model& Model;

    // clang-format off
    auto GetNode      (int idx) const { return TinyGltfNodeWrapper      {Model.nodes      [idx]}; }
    auto GetMesh      (int idx) const { return TinyGltfMeshWrapper      {Model.meshes     [idx]}; }
    auto GetAccessor  (int idx) const { return TinyGltfAccessorWrapper  {Model.accessors  [idx]}; }
    auto GetCamera    (int idx) const { return TinyGltfCameraWrapper    {Model.cameras    [idx]}; }
    auto GetBufferView(int idx) const { return TinyGltfBufferViewWrapper{Model.bufferViews[idx]}; }
    auto GetBuffer    (int idx) const { return TinyGltfBufferWrapper    {Model.buffers    [idx]}; }

    auto GetSkinCount()      const { return Model.skins.size(); }
    auto GetSkin(size_t idx) const { return TinyGltfSkinWrapper{Model.skins[idx]}; }

    auto GetAnimationCount()      const { return Model.animations.size(); }
    auto GetAnimation(size_t idx) const { return TinyGltfAnimationWrapper{Model.animations[idx]}; }
    // clang-format off
};

auto TinyGltfAccessorWrapper::GetByteStride(const TinyGltfBufferViewWrapper& View) const
{
    return Accessor.ByteStride(View.View);
}


struct TextureInitData : public ObjectBase<IObject>
{
    TextureInitData(IReferenceCounters* pRefCounters) :
        ObjectBase<IObject>{pRefCounters}
    {}

    struct LevelData
    {
        std::vector<unsigned char> Data;

        TextureSubResData SubResData;

        Uint32 Width  = 0;
        Uint32 Height = 0;
    };
    std::vector<LevelData> Levels;

    RefCntAutoPtr<ITexture> pStagingTex;

    void GenerateMipLevels(Uint32 StartMipLevel, TEXTURE_FORMAT Format)
    {
        VERIFY_EXPR(StartMipLevel > 0);

        const auto& FmtAttribs = GetTextureFormatAttribs(Format);

        // Note: this will work even when NumMipLevels is greater than
        //       finest mip resolution. All coarser mip levels will be 1x1.
        for (Uint32 mip = StartMipLevel; mip < Levels.size(); ++mip)
        {
            auto&       Level     = Levels[mip];
            const auto& FineLevel = Levels[mip - 1];

            // Note that we can't use GetMipLevelProperties here
            Level.Width  = AlignUp(std::max(FineLevel.Width / 2u, 1u), Uint32{FmtAttribs.BlockWidth});
            Level.Height = AlignUp(std::max(FineLevel.Height / 2u, 1u), Uint32{FmtAttribs.BlockHeight});

            Level.SubResData.Stride =
                Uint64{Level.Width} / Uint64{FmtAttribs.BlockWidth} * Uint64{FmtAttribs.ComponentSize} *
                (FmtAttribs.ComponentType != COMPONENT_TYPE_COMPRESSED ? Uint64{FmtAttribs.NumComponents} : 1);
            const auto MipSize = Level.SubResData.Stride * Level.Height / Uint32{FmtAttribs.BlockHeight};

            Level.Data.resize(static_cast<size_t>(MipSize));
            Level.SubResData.pData = Level.Data.data();

            if (FmtAttribs.ComponentType != COMPONENT_TYPE_COMPRESSED)
            {
                ComputeMipLevel({Format, FineLevel.Width, FineLevel.Height,
                                 FineLevel.Data.data(), StaticCast<size_t>(FineLevel.SubResData.Stride),
                                 Level.Data.data(), StaticCast<size_t>(Level.SubResData.Stride)});
            }
            else
            {
                UNSUPPORTED("Mip generation for compressed formats is not currently implemented");
            }
        }
    }
};

} // namespace


static RefCntAutoPtr<TextureInitData> PrepareGLTFTextureInitData(
    const tinygltf::Image& gltfimage,
    float                  AlphaCutoff,
    Uint32                 NumMipLevels)
{
    VERIFY_EXPR(!gltfimage.image.empty());
    VERIFY_EXPR(gltfimage.width > 0 && gltfimage.height > 0 && gltfimage.component > 0);

    RefCntAutoPtr<TextureInitData> UpdateInfo{MakeNewRCObj<TextureInitData>()()};

    auto& Levels = UpdateInfo->Levels;
    Levels.resize(NumMipLevels);

    auto& Level0  = Levels[0];
    Level0.Width  = static_cast<Uint32>(gltfimage.width);
    Level0.Height = static_cast<Uint32>(gltfimage.height);

    auto& Level0Stride{Level0.SubResData.Stride};
    Level0Stride = Uint64{Level0.Width} * 4;

    if (gltfimage.component == 3)
    {
        Level0.Data.resize(static_cast<size_t>(Level0Stride * gltfimage.height));

        // Due to depressing performance of iterators in debug MSVC we have to use raw pointers here
        const auto* rgb  = gltfimage.image.data();
        auto*       rgba = Level0.Data.data();
        for (int i = 0; i < gltfimage.width * gltfimage.height; ++i)
        {
            rgba[0] = rgb[0];
            rgba[1] = rgb[1];
            rgba[2] = rgb[2];
            rgba[3] = 255;

            rgba += 4;
            rgb += 3;
        }
        VERIFY_EXPR(rgb == gltfimage.image.data() + gltfimage.image.size());
        VERIFY_EXPR(rgba == Level0.Data.data() + Level0.Data.size());
    }
    else if (gltfimage.component == 4)
    {
        if (AlphaCutoff > 0)
        {
            Level0.Data.resize(static_cast<size_t>(Level0Stride * gltfimage.height));

            // Remap alpha channel using the following formula to improve mip maps:
            //
            //      A_new = max(A_old; 1/3 * A_old + 2/3 * CutoffThreshold)
            //
            // https://asawicki.info/articles/alpha_test.php5

            VERIFY_EXPR(AlphaCutoff > 0 && AlphaCutoff <= 1);
            AlphaCutoff *= 255.f;

            // Due to depressing performance of iterators in debug MSVC we have to use raw pointers here
            const auto* src = gltfimage.image.data();
            auto*       dst = Level0.Data.data();
            for (int i = 0; i < gltfimage.width * gltfimage.height; ++i)
            {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = std::max(src[3], static_cast<Uint8>(std::min(1.f / 3.f * src[3] + 2.f / 3.f * AlphaCutoff, 255.f)));

                src += 4;
                dst += 4;
            }
            VERIFY_EXPR(src == gltfimage.image.data() + gltfimage.image.size());
            VERIFY_EXPR(dst == Level0.Data.data() + Level0.Data.size());
        }
        else
        {
            VERIFY_EXPR(gltfimage.image.size() == Level0Stride * gltfimage.height);
            Level0.Data = std::move(gltfimage.image);
        }
    }
    else
    {
        UNEXPECTED("Unexpected number of color components in gltf image: ", gltfimage.component);
    }
    Level0.SubResData.pData = Level0.Data.data();

    UpdateInfo->GenerateMipLevels(1, TEX_FORMAT_RGBA8_UNORM);

    return UpdateInfo;
}

Mesh::Mesh(const float4x4& matrix)
{
    Transforms.matrix = matrix;
}



float4x4 Node::LocalMatrix() const
{
    // Translation, rotation, and scale properties and local space transformation are
    // mutually exclusive in GLTF.
    // We, however, may use non-trivial Matrix with TRS to apply transform to a model.
    return float4x4::Scale(Scale) * Rotation.ToMatrix() * float4x4::Translation(Translation) * Matrix;
}

float4x4 Node::GetMatrix() const
{
    auto mat = LocalMatrix();

    for (auto* p = Parent; p != nullptr; p = p->Parent)
    {
        mat = mat * p->LocalMatrix();
    }
    return mat;
}

void Node::UpdateTransforms()
{
    const auto NodeTransform = (pMesh || pCamera) ? GetMatrix() : float4x4::Identity();
    if (pMesh)
    {
        pMesh->Transforms.matrix = NodeTransform;
        if (pSkin != nullptr)
        {
            // Update join matrices
            auto InverseTransform = pMesh->Transforms.matrix.Inverse(); // TODO: do not use inverse transform here
            if (pMesh->Transforms.jointMatrices.size() != pSkin->Joints.size())
                pMesh->Transforms.jointMatrices.resize(pSkin->Joints.size());
            for (size_t i = 0; i < pSkin->Joints.size(); i++)
            {
                auto* JointNode = pSkin->Joints[i];
                pMesh->Transforms.jointMatrices[i] =
                    pSkin->InverseBindMatrices[i] * JointNode->GetMatrix() * InverseTransform;
            }
        }
    }

    if (pCamera)
    {
        pCamera->matrix = NodeTransform;
    }

    for (auto& child : Children)
    {
        child->UpdateTransforms();
    }
}



Model::Model(const ModelCreateInfo& CI)
{
    DEV_CHECK_ERR(CI.IndexType == VT_UINT16 || CI.IndexType == VT_UINT32, "Invalid index type");

    if (CI.VertexAttributes != nullptr)
    {
        DEV_CHECK_ERR(CI.NumVertexAttributes > 0, "There should be at least one vertex attribute");
        VertexAttributes.assign(CI.VertexAttributes, CI.VertexAttributes + CI.NumVertexAttributes);
    }
    else
    {
        VertexAttributes.assign(DefaultVertexAttributes.begin(), DefaultVertexAttributes.end());
    }

    Uint32 MaxBufferId = 0;
    for (const auto& Attrib : VertexAttributes)
    {
        DEV_CHECK_ERR(Attrib.Name != nullptr, "Attribute name must not be null");
        DEV_CHECK_ERR(Attrib.ValueType != VT_UNDEFINED, "Undefined attribute value type");
        DEV_CHECK_ERR(Attrib.NumComponents != 0, "The number of components must not be null");

        MaxBufferId = std::max<Uint32>(MaxBufferId, Attrib.BufferId);
    }
    Buffers.resize(size_t{MaxBufferId} + 1 + 1);

    for (auto& Attrib : VertexAttributes)
    {
        auto& ElementStride = Buffers[Attrib.BufferId].ElementStride;
        if (Attrib.RelativeOffset == VertexAttributeDesc{}.RelativeOffset)
        {
            Attrib.RelativeOffset = ElementStride;
        }
        else
        {
            DEV_CHECK_ERR(Attrib.RelativeOffset >= ElementStride, "Invalid offset: the attribute overlaps with previous attributes.");
        }

        ElementStride = Attrib.RelativeOffset + GetValueSize(Attrib.ValueType) * Attrib.NumComponents;
    }

#ifdef DILIGENT_DEBUG
    if (CI.VertexAttributes == nullptr)
    {
        VERIFY_EXPR(Buffers.size() == 3);
        VERIFY_EXPR(Buffers[0].ElementStride == sizeof(VertexBasicAttribs));
        VERIFY_EXPR(Buffers[1].ElementStride == sizeof(VertexSkinAttribs));
    }
#endif

    Buffers.back().ElementStride = CI.IndexType == VT_UINT32 ? 4 : 2;
}

Model::Model(IRenderDevice*         pDevice,
             IDeviceContext*        pContext,
             const ModelCreateInfo& CI) :
    Model{CI}
{
    LoadFromFile(pDevice, pContext, CI);
}

Model::Model() noexcept
{
}

Model::~Model()
{
}

static float GetTextureAlphaCutoffValue(const std::vector<tinygltf::Material>& gltf_materials, int TextureIndex)
{
    float AlphaCutoff = -1.f;

    for (const auto& gltf_mat : gltf_materials)
    {
        auto base_color_tex_it = gltf_mat.values.find("baseColorTexture");
        if (base_color_tex_it == gltf_mat.values.end())
        {
            // The material has no base texture
            continue;
        }

        if (base_color_tex_it->second.TextureIndex() != TextureIndex)
        {
            // The material does not use this texture
            continue;
        }

        auto alpha_mode_it = gltf_mat.additionalValues.find("alphaMode");
        if (alpha_mode_it == gltf_mat.additionalValues.end())
        {
            // The material uses this texture, but it is not an alpha-blended or an alpha-cut material
            AlphaCutoff = 0.f;
            continue;
        }

        const tinygltf::Parameter& param = alpha_mode_it->second;
        if (param.string_value == "MASK")
        {
            auto MaterialAlphaCutoff = 0.5f;
            auto alpha_cutoff_it     = gltf_mat.additionalValues.find("alphaCutoff");
            if (alpha_cutoff_it != gltf_mat.additionalValues.end())
            {
                MaterialAlphaCutoff = static_cast<float>(alpha_cutoff_it->second.Factor());
            }

            if (AlphaCutoff < 0)
            {
                AlphaCutoff = MaterialAlphaCutoff;
            }
            else if (AlphaCutoff != MaterialAlphaCutoff)
            {
                if (AlphaCutoff == 0)
                {
                    LOG_WARNING_MESSAGE("Texture ", TextureIndex,
                                        " is used in an alpha-cut material with threshold ", MaterialAlphaCutoff,
                                        " as well as in a non-alpha-cut material."
                                        " Alpha remapping to improve mipmap generation will be disabled.");
                }
                else
                {
                    LOG_WARNING_MESSAGE("Texture ", TextureIndex,
                                        " is used in alpha-cut materials with different cutoff thresholds (", AlphaCutoff, ", ", MaterialAlphaCutoff,
                                        "). Alpha remapping to improve mipmap generation will use ",
                                        AlphaCutoff, '.');
                }
            }
        }
        else
        {
            // The material is not an alpha-cut material
            if (AlphaCutoff > 0)
            {
                LOG_WARNING_MESSAGE("Texture ", TextureIndex,
                                    " is used in an alpha-cut material as well as in a non-alpha-cut material."
                                    " Alpha remapping to improve mipmap generation will be disabled.");
            }
            AlphaCutoff = 0.f;
        }
    }

    return std::max(AlphaCutoff, 0.f);
}

void Model::AddTexture(IRenderDevice*                         pDevice,
                       TextureCacheType*                      pTextureCache,
                       ResourceManager*                       pResourceMgr,
                       const tinygltf::Image&                 gltf_image,
                       int                                    gltf_sampler,
                       const std::vector<tinygltf::Material>& gltf_materials,
                       const std::string&                     CacheId)
{
    TextureInfo TexInfo;
    if (!CacheId.empty())
    {
        if (pResourceMgr != nullptr)
        {
            TexInfo.pAtlasSuballocation = pResourceMgr->FindAllocation(CacheId.c_str());
            if (TexInfo.pAtlasSuballocation)
            {
                // Note that the texture may appear in the cache after the call to LoadImageData because
                // it can be loaded by another thread
                VERIFY_EXPR(gltf_image.width == -1 || gltf_image.width == static_cast<int>(TexInfo.pAtlasSuballocation->GetSize().x));
                VERIFY_EXPR(gltf_image.height == -1 || gltf_image.height == static_cast<int>(TexInfo.pAtlasSuballocation->GetSize().y));
            }
        }
        else if (pTextureCache != nullptr)
        {
            std::lock_guard<std::mutex> Lock{pTextureCache->TexturesMtx};

            auto it = pTextureCache->Textures.find(CacheId);
            if (it != pTextureCache->Textures.end())
            {
                TexInfo.pTexture = it->second.Lock();
                if (!TexInfo.pTexture)
                {
                    // Image width and height (or pixel_type for dds/ktx) are initialized by LoadImageData()
                    // if the texture is found in the cache.
                    if ((gltf_image.width > 0 && gltf_image.height > 0) ||
                        (gltf_image.pixel_type == IMAGE_FILE_FORMAT_DDS || gltf_image.pixel_type == IMAGE_FILE_FORMAT_KTX))
                    {
                        UNEXPECTED("Stale textures should not be found in the texture cache because we hold strong references. "
                                   "This must be an unexpected effect of loading resources from multiple threads or a bug.");
                    }
                    else
                    {
                        pTextureCache->Textures.erase(it);
                    }
                }
            }
        }
    }

    if (!TexInfo.IsValid())
    {
        RefCntAutoPtr<ISampler> pSampler;
        if (gltf_sampler == -1)
        {
            // No sampler specified, use a default one
            pDevice->CreateSampler(Sam_LinearWrap, &pSampler);
        }
        else
        {
            pSampler = TextureSamplers[gltf_sampler];
        }

        // Check if the texture is used in an alpha-cut material
        const float AlphaCutoff = GetTextureAlphaCutoffValue(gltf_materials, static_cast<int>(Textures.size()));

        if (gltf_image.width > 0 && gltf_image.height > 0)
        {
            if (pResourceMgr != nullptr)
            {
                // No reference
                const TextureDesc AtlasDesc = pResourceMgr->GetAtlasDesc(TEX_FORMAT_RGBA8_UNORM);

                // Load all mip levels.
                auto pInitData = PrepareGLTFTextureInitData(gltf_image, AlphaCutoff, AtlasDesc.MipLevels);

                // pInitData will be atomically set in the allocation before any other thread may be able to
                // access it.
                // Note that it is possible that more than one thread prepares pInitData for the same allocation.
                // It it also possible that multiple instances of the same allocation are created before the first
                // is added to the cache. This is all OK though.
                TexInfo.pAtlasSuballocation =
                    pResourceMgr->AllocateTextureSpace(TEX_FORMAT_RGBA8_UNORM, gltf_image.width, gltf_image.height, CacheId.c_str(), pInitData);

                VERIFY_EXPR(TexInfo.pAtlasSuballocation->GetAtlas()->GetAtlasDesc().MipLevels == AtlasDesc.MipLevels);
            }
            else
            {
                TextureDesc TexDesc;
                TexDesc.Name      = "GLTF Texture";
                TexDesc.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
                TexDesc.Usage     = USAGE_DEFAULT;
                TexDesc.BindFlags = BIND_SHADER_RESOURCE;
                TexDesc.Width     = gltf_image.width;
                TexDesc.Height    = gltf_image.height;
                TexDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
                TexDesc.MipLevels = 0;
                TexDesc.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;

                pDevice->CreateTexture(TexDesc, nullptr, &TexInfo.pTexture);
                TexInfo.pTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(pSampler);

                // Load only the lowest mip level; other mip levels will be generated on the GPU.
                auto pTexInitData = PrepareGLTFTextureInitData(gltf_image, AlphaCutoff, 1);
                TexInfo.pTexture->SetUserData(pTexInitData);
            }
        }
        else if (gltf_image.pixel_type == IMAGE_FILE_FORMAT_DDS || gltf_image.pixel_type == IMAGE_FILE_FORMAT_KTX)
        {
            RefCntAutoPtr<TextureInitData> pTexInitData{MakeNewRCObj<TextureInitData>()()};

            // Create the texture from raw bits
            RefCntAutoPtr<ITextureLoader> pTexLoader;

            TextureLoadInfo LoadInfo;
            LoadInfo.Name = "GLTF texture";
            if (pResourceMgr != nullptr)
            {
                LoadInfo.Usage          = USAGE_STAGING;
                LoadInfo.BindFlags      = BIND_NONE;
                LoadInfo.CPUAccessFlags = CPU_ACCESS_WRITE;
            }
            CreateTextureLoaderFromMemory(gltf_image.image.data(), gltf_image.image.size(), static_cast<IMAGE_FILE_FORMAT>(gltf_image.pixel_type), false /*MakeDataCopy*/, LoadInfo, &pTexLoader);
            if (pTexLoader)
            {
                if (pResourceMgr == nullptr)
                {
                    pTexLoader->CreateTexture(pDevice, &TexInfo.pTexture);
                    // Set empty init data to indicate that the texture needs to be transitioned to correct state
                    TexInfo.pTexture->SetUserData(pTexInitData);
                }
                else
                {
                    const auto& TexDesc = pTexLoader->GetTextureDesc();

                    // pTexInitData will be atomically set in the allocation before any other thread may be able to
                    // access it.
                    // Note that it is possible that more than one thread prepares pTexInitData for the same allocation.
                    // It it also possible that multiple instances of the same allocation are created before the first
                    // is added to the cache. This is all OK though.
                    TexInfo.pAtlasSuballocation = pResourceMgr->AllocateTextureSpace(TexDesc.Format, TexDesc.Width, TexDesc.Height, CacheId.c_str(), pTexInitData);

                    // NB: create staging texture to save work in the main thread when
                    //     this function is called from a worker thread
                    pTexLoader->CreateTexture(pDevice, &pTexInitData->pStagingTex);
                }
            }
        }

        if (pResourceMgr == nullptr && !TexInfo.pTexture)
        {
            // Create stub texture
            TextureDesc TexDesc;
            TexDesc.Name      = "Checkerboard stub texture";
            TexDesc.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
            TexDesc.Width     = 32;
            TexDesc.Height    = 32;
            TexDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
            TexDesc.MipLevels = 1;
            TexDesc.Usage     = USAGE_DEFAULT;
            TexDesc.BindFlags = BIND_SHADER_RESOURCE;

            RefCntAutoPtr<TextureInitData> pTexInitData{MakeNewRCObj<TextureInitData>()()};

            pTexInitData->Levels.resize(1);
            auto& Level0  = pTexInitData->Levels[0];
            Level0.Width  = TexDesc.Width;
            Level0.Height = TexDesc.Height;

            auto& Level0Stride{Level0.SubResData.Stride};
            Level0Stride = Uint64{Level0.Width} * 4;
            Level0.Data.resize(static_cast<size_t>(Level0Stride * TexDesc.Height));
            Level0.SubResData.pData = Level0.Data.data();
            GenerateCheckerBoardPattern(TexDesc.Width, TexDesc.Height, TexDesc.Format, 4, 4, Level0.Data.data(), Level0Stride);

            pDevice->CreateTexture(TexDesc, nullptr, &TexInfo.pTexture);
            TexInfo.pTexture->SetUserData(pTexInitData);
        }

        if (TexInfo.pTexture && pTextureCache != nullptr)
        {
            std::lock_guard<std::mutex> Lock{pTextureCache->TexturesMtx};
            pTextureCache->Textures.emplace(CacheId, TexInfo.pTexture);
        }
    }

    Textures.emplace_back(std::move(TexInfo));
}


void Model::LoadTextures(IRenderDevice*         pDevice,
                         const tinygltf::Model& gltf_model,
                         const std::string&     BaseDir,
                         TextureCacheType*      pTextureCache,
                         ResourceManager*       pResourceMgr)
{
    for (const tinygltf::Texture& gltf_tex : gltf_model.textures)
    {
        const auto& gltf_image = gltf_model.images[gltf_tex.source];
        const auto  CacheId    = !gltf_image.uri.empty() ? FileSystem::SimplifyPath((BaseDir + gltf_image.uri).c_str()) : "";

        AddTexture(pDevice, pTextureCache, pResourceMgr, gltf_image, gltf_tex.sampler, gltf_model.materials, CacheId);
    }
}

void Model::PrepareGPUResources(IRenderDevice* pDevice, IDeviceContext* pCtx)
{
    if (GPUDataInitialized.load())
        return;

    std::vector<StateTransitionDesc> Barriers;

    for (Uint32 i = 0; i < Textures.size(); ++i)
    {
        auto&     DstTexInfo = Textures[i];
        ITexture* pTexture   = nullptr;

        RefCntAutoPtr<TextureInitData> pInitData;
        if (DstTexInfo.pAtlasSuballocation)
        {
            pTexture  = DstTexInfo.pAtlasSuballocation->GetAtlas()->GetTexture(pDevice, pCtx);
            pInitData = ClassPtrCast<TextureInitData>(DstTexInfo.pAtlasSuballocation->GetUserData());
            // User data is only set when the allocation is created, so no other
            // thread can call SetUserData() in parallel.
            DstTexInfo.pAtlasSuballocation->SetUserData(nullptr);
        }
        else if (DstTexInfo.pTexture)
        {
            pTexture  = DstTexInfo.pTexture;
            pInitData = ClassPtrCast<TextureInitData>(pTexture->GetUserData());
            // User data is only set when the texture is created, so no other
            // thread can call SetUserData() in parallel.
            pTexture->SetUserData(nullptr);
        }

        if (!pTexture)
            continue;

        if (pInitData == nullptr)
        {
            // Shared texture has already been initialized by another model
            continue;
        }

        const auto& Levels      = pInitData->Levels;
        auto&       pStagingTex = pInitData->pStagingTex;
        const auto  DstSlice    = DstTexInfo.pAtlasSuballocation ? DstTexInfo.pAtlasSuballocation->GetSlice() : 0;
        const auto& TexDesc     = pTexture->GetDesc();

        if (!Levels.empty() || pStagingTex)
        {
            Uint32 DstX = 0;
            Uint32 DstY = 0;
            if (DstTexInfo.pAtlasSuballocation)
            {
                const auto& Origin = DstTexInfo.pAtlasSuballocation->GetOrigin();

                DstX = Origin.x;
                DstY = Origin.y;
            }

            if (!Levels.empty())
            {
                VERIFY(!pStagingTex, "Staging texture and levels are mutually exclusive");
                VERIFY_EXPR(Levels.size() == 1 || Levels.size() == TexDesc.MipLevels);
                for (Uint32 mip = 0; mip < Levels.size(); ++mip)
                {
                    const auto& Level = Levels[mip];

                    Box UpdateBox;
                    UpdateBox.MinX = DstX >> mip;
                    UpdateBox.MaxX = UpdateBox.MinX + Level.Width;
                    UpdateBox.MinY = DstY >> mip;
                    UpdateBox.MaxY = UpdateBox.MinY + Level.Height;
                    pCtx->UpdateTexture(pTexture, mip, DstSlice, UpdateBox, Level.SubResData, RESOURCE_STATE_TRANSITION_MODE_NONE, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                }

                if (Levels.size() == 1 && TexDesc.MipLevels > 1 && DstTexInfo.pTexture)
                {
                    // Only generate mips when texture atlas is not used
                    pCtx->GenerateMips(pTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
                }
            }
            else if (pStagingTex)
            {
                VERIFY(DstTexInfo.pAtlasSuballocation, "Staging texture is expected to be used with the atlas");
                const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
                const auto& SrcTexDesc = pStagingTex->GetDesc();

                auto SrcMips = std::min(SrcTexDesc.MipLevels, TexDesc.MipLevels);
                if (FmtAttribs.ComponentType == COMPONENT_TYPE_COMPRESSED)
                {
                    // Do not copy mip levels that are smaller than the block size
                    for (; SrcMips > 0; --SrcMips)
                    {
                        const auto MipProps = GetMipLevelProperties(SrcTexDesc, SrcMips - 1);
                        if (MipProps.LogicalWidth >= FmtAttribs.BlockWidth &&
                            MipProps.LogicalHeight >= FmtAttribs.BlockHeight)
                            break;
                    }
                }
                for (Uint32 mip = 0; mip < SrcMips; ++mip)
                {
                    CopyTextureAttribs CopyAttribs{pStagingTex, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
                    CopyAttribs.SrcMipLevel = mip;
                    CopyAttribs.DstMipLevel = mip;
                    CopyAttribs.DstSlice    = DstSlice;
                    CopyAttribs.DstX        = DstX >> mip;
                    CopyAttribs.DstY        = DstY >> mip;
                    pCtx->CopyTexture(CopyAttribs);
                }
            }
        }
        else
        {
            // Texture is already initialized
        }

        if (DstTexInfo.pTexture)
        {
            // Note that we may need to transition a texture even if it has been fully initialized,
            // as is the case with KTX/DDS textures.
            VERIFY_EXPR(pTexture == DstTexInfo.pTexture);
            Barriers.emplace_back(StateTransitionDesc{pTexture, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE});
        }
    }

    for (size_t BuffId = 0; BuffId < Buffers.size(); ++BuffId)
    {
        auto&    BuffInfo = Buffers[BuffId];
        IBuffer* pBuffer  = nullptr;
        Uint32   Offset   = 0;

        RefCntAutoPtr<IDataBlob> pInitData;
        if (BuffInfo.pSuballocation)
        {
            pBuffer   = BuffInfo.pSuballocation->GetAllocator()->GetBuffer(pDevice, pCtx);
            Offset    = BuffInfo.pSuballocation->GetOffset();
            pInitData = RefCntAutoPtr<IDataBlob>{BuffInfo.pSuballocation->GetUserData(), IID_DataBlob};
            BuffInfo.pSuballocation->SetUserData(nullptr);
        }
        else if (BuffInfo.pBuffer)
        {
            pBuffer   = BuffInfo.pBuffer;
            pInitData = RefCntAutoPtr<IDataBlob>{pBuffer->GetUserData(), IID_DataBlob};
            pBuffer->SetUserData(nullptr);
        }
        else
        {
            return;
        }

        if (pInitData)
        {
            pCtx->UpdateBuffer(pBuffer, Offset, static_cast<Uint32>(pInitData->GetSize()), pInitData->GetConstDataPtr(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            if (BuffInfo.pBuffer != nullptr)
            {
                VERIFY_EXPR(BuffInfo.pBuffer == pBuffer);
                Barriers.emplace_back(StateTransitionDesc{pBuffer, RESOURCE_STATE_UNKNOWN, BuffId == Buffers.size() - 1 ? RESOURCE_STATE_INDEX_BUFFER : RESOURCE_STATE_VERTEX_BUFFER, STATE_TRANSITION_FLAG_UPDATE_STATE});
            }
        }
    };

    if (!Barriers.empty())
        pCtx->TransitionResourceStates(static_cast<Uint32>(Barriers.size()), Barriers.data());

    GPUDataInitialized.store(true);
}

namespace
{

TEXTURE_ADDRESS_MODE GetWrapMode(int32_t wrapMode)
{
    switch (wrapMode)
    {
        case 10497:
            return TEXTURE_ADDRESS_WRAP;
        case 33071:
            return TEXTURE_ADDRESS_CLAMP;
        case 33648:
            return TEXTURE_ADDRESS_MIRROR;
        default:
            LOG_WARNING_MESSAGE("Unknown gltf address wrap mode: ", wrapMode, ". Defaulting to WRAP.");
            return TEXTURE_ADDRESS_WRAP;
    }
}

std::pair<FILTER_TYPE, FILTER_TYPE> GetFilterMode(int32_t filterMode)
{
    switch (filterMode)
    {
        case 9728: // NEAREST
            return {FILTER_TYPE_POINT, FILTER_TYPE_POINT};
        case 9729: // LINEAR
            return {FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR};
        case 9984: // NEAREST_MIPMAP_NEAREST
            return {FILTER_TYPE_POINT, FILTER_TYPE_POINT};
        case 9985: // LINEAR_MIPMAP_NEAREST
            return {FILTER_TYPE_LINEAR, FILTER_TYPE_POINT};
        case 9986:                                           // NEAREST_MIPMAP_LINEAR
            return {FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR}; // use linear min filter instead as point makes no sesne
        case 9987:                                           // LINEAR_MIPMAP_LINEAR
            return {FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR};
        default:
            LOG_WARNING_MESSAGE("Unknown gltf filter mode: ", filterMode, ". Defaulting to linear.");
            return {FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR};
    }
}

} // namespace

void Model::LoadTextureSamplers(IRenderDevice* pDevice, const tinygltf::Model& gltf_model)
{
    for (const tinygltf::Sampler& smpl : gltf_model.samplers)
    {
        SamplerDesc SamDesc;
        SamDesc.MagFilter = GetFilterMode(smpl.magFilter).first;
        auto MinMipFilter = GetFilterMode(smpl.minFilter);
        SamDesc.MinFilter = MinMipFilter.first;
        SamDesc.MipFilter = MinMipFilter.second;
        SamDesc.AddressU  = GetWrapMode(smpl.wrapS);
        SamDesc.AddressV  = GetWrapMode(smpl.wrapT);
        SamDesc.AddressW  = SamDesc.AddressV;
        RefCntAutoPtr<ISampler> pSampler;
        pDevice->CreateSampler(SamDesc, &pSampler);
        TextureSamplers.push_back(std::move(pSampler));
    }
}


void Model::LoadMaterials(const tinygltf::Model& gltf_model, const ModelCreateInfo::MaterialLoadCallbackType& MaterialLoadCallback)
{
    for (const tinygltf::Material& gltf_mat : gltf_model.materials)
    {
        Material Mat;

        struct TextureParameterInfo
        {
            const Material::TEXTURE_ID    TextureId;
            float&                        UVSelector;
            float4&                       UVScaleBias;
            float&                        Slice;
            const char* const             TextureName;
            const tinygltf::ParameterMap& Params;
        };
        // clang-format off
        std::array<TextureParameterInfo, 5> TextureParams =
        {
            TextureParameterInfo{Material::TEXTURE_ID_BASE_COLOR,    Mat.Attribs.BaseColorUVSelector,          Mat.Attribs.BaseColorUVScaleBias,          Mat.Attribs.BaseColorSlice,          "baseColorTexture",         gltf_mat.values},
            TextureParameterInfo{Material::TEXTURE_ID_PHYSICAL_DESC, Mat.Attribs.PhysicalDescriptorUVSelector, Mat.Attribs.PhysicalDescriptorUVScaleBias, Mat.Attribs.PhysicalDescriptorSlice, "metallicRoughnessTexture", gltf_mat.values},
            TextureParameterInfo{Material::TEXTURE_ID_NORMAL_MAP,    Mat.Attribs.NormalUVSelector,             Mat.Attribs.NormalUVScaleBias,             Mat.Attribs.NormalSlice,             "normalTexture",            gltf_mat.additionalValues},
            TextureParameterInfo{Material::TEXTURE_ID_OCCLUSION,     Mat.Attribs.OcclusionUVSelector,          Mat.Attribs.OcclusionUVScaleBias,          Mat.Attribs.OcclusionSlice,          "occlusionTexture",         gltf_mat.additionalValues},
            TextureParameterInfo{Material::TEXTURE_ID_EMISSIVE,      Mat.Attribs.EmissiveUVSelector,           Mat.Attribs.EmissiveUVScaleBias,           Mat.Attribs.EmissiveSlice,           "emissiveTexture",          gltf_mat.additionalValues}
        };
        // clang-format on

        for (const auto& Param : TextureParams)
        {
            auto tex_it = Param.Params.find(Param.TextureName);
            if (tex_it != Param.Params.end())
            {
                Mat.TextureIds[Param.TextureId] = tex_it->second.TextureIndex();
                Param.UVSelector                = static_cast<float>(tex_it->second.TextureTexCoord());
            }
        }

        auto ReadFactor = [](float& Factor, const tinygltf::ParameterMap& Params, const char* Name) //
        {
            auto it = Params.find(Name);
            if (it != Params.end())
            {
                Factor = static_cast<float>(it->second.Factor());
            }
        };
        ReadFactor(Mat.Attribs.RoughnessFactor, gltf_mat.values, "roughnessFactor");
        ReadFactor(Mat.Attribs.MetallicFactor, gltf_mat.values, "metallicFactor");

        auto ReadColorFactor = [](float4& Factor, const tinygltf::ParameterMap& Params, const char* Name) //
        {
            auto it = Params.find(Name);
            if (it != Params.end())
            {
                Factor = float4::MakeVector(it->second.ColorFactor().data());
            }
        };

        ReadColorFactor(Mat.Attribs.BaseColorFactor, gltf_mat.values, "baseColorFactor");
        ReadColorFactor(Mat.Attribs.EmissiveFactor, gltf_mat.additionalValues, "emissiveFactor");

        {
            auto alpha_mode_it = gltf_mat.additionalValues.find("alphaMode");
            if (alpha_mode_it != gltf_mat.additionalValues.end())
            {
                const tinygltf::Parameter& param = alpha_mode_it->second;
                if (param.string_value == "BLEND")
                {
                    Mat.Attribs.AlphaMode = Material::ALPHA_MODE_BLEND;
                }
                if (param.string_value == "MASK")
                {
                    Mat.Attribs.AlphaMode   = Material::ALPHA_MODE_MASK;
                    Mat.Attribs.AlphaCutoff = 0.5f;
                }
            }
        }

        ReadFactor(Mat.Attribs.AlphaCutoff, gltf_mat.additionalValues, "alphaCutoff");

        {
            auto double_sided_it = gltf_mat.additionalValues.find("doubleSided");
            if (double_sided_it != gltf_mat.additionalValues.end())
            {
                Mat.DoubleSided = double_sided_it->second.bool_value;
            }
        }

        Mat.Attribs.Workflow = Material::PBR_WORKFLOW_METALL_ROUGH;

        // Extensions
        // @TODO: Find out if there is a nicer way of reading these properties with recent tinygltf headers
        {
            auto ext_it = gltf_mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (ext_it != gltf_mat.extensions.end())
            {
                if (ext_it->second.Has("specularGlossinessTexture"))
                {
                    auto index       = ext_it->second.Get("specularGlossinessTexture").Get("index");
                    auto texCoordSet = ext_it->second.Get("specularGlossinessTexture").Get("texCoord");

                    Mat.TextureIds[Material::TEXTURE_ID_PHYSICAL_DESC] = index.Get<int>();
                    Mat.Attribs.PhysicalDescriptorUVSelector           = static_cast<float>(texCoordSet.Get<int>());

                    Mat.Attribs.Workflow = Material::PBR_WORKFLOW_SPEC_GLOSS;
                }

                if (ext_it->second.Has("diffuseTexture"))
                {
                    auto index       = ext_it->second.Get("diffuseTexture").Get("index");
                    auto texCoordSet = ext_it->second.Get("diffuseTexture").Get("texCoord");

                    Mat.TextureIds[Material::TEXTURE_ID_BASE_COLOR] = index.Get<int>();
                    Mat.Attribs.BaseColorUVSelector                 = static_cast<float>(texCoordSet.Get<int>());
                }

                if (ext_it->second.Has("diffuseFactor"))
                {
                    auto factor = ext_it->second.Get("diffuseFactor");
                    for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                    {
                        const auto val = factor.Get(i);
                        Mat.Attribs.BaseColorFactor[i] =
                            val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                    }
                }

                if (ext_it->second.Has("specularFactor"))
                {
                    auto factor = ext_it->second.Get("specularFactor");
                    for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                    {
                        const auto val = factor.Get(i);
                        Mat.Attribs.SpecularFactor[i] =
                            val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                    }
                }
            }
        }

        for (const auto& Param : TextureParams)
        {
            auto TexIndex = Mat.TextureIds[Param.TextureId];
            if (TexIndex >= 0)
            {
                const auto& TexInfo = Textures[TexIndex];
                if (TexInfo.pAtlasSuballocation)
                {
                    Param.UVScaleBias = TexInfo.pAtlasSuballocation->GetUVScaleBias();
                    Param.Slice       = static_cast<float>(TexInfo.pAtlasSuballocation->GetSlice());
                }
            }
        }

        if (MaterialLoadCallback != nullptr)
            MaterialLoadCallback(gltf_mat, Mat);

        Materials.push_back(Mat);
    }

    // Push a default material at the end of the list for meshes with no material assigned
    Materials.push_back(Material{});
}

namespace Callbacks
{

namespace
{

struct LoaderData
{
    TextureCacheType* const pTextureCache;
    ResourceManager* const  pResourceMgr;

    std::vector<RefCntAutoPtr<IObject>> TexturesHold;

    std::string BaseDir;

    ModelCreateInfo::FileExistsCallbackType    FileExists    = nullptr;
    ModelCreateInfo::ReadWholeFileCallbackType ReadWholeFile = nullptr;
};


bool LoadImageData(tinygltf::Image*     gltf_image,
                   const int            gltf_image_idx,
                   std::string*         error,
                   std::string*         warning,
                   int                  req_width,
                   int                  req_height,
                   const unsigned char* image_data,
                   int                  size,
                   void*                user_data)
{
    (void)warning;

    auto* pLoaderData = static_cast<LoaderData*>(user_data);
    if (pLoaderData != nullptr)
    {
        const auto CacheId = !gltf_image->uri.empty() ? FileSystem::SimplifyPath((pLoaderData->BaseDir + gltf_image->uri).c_str()) : "";

        if (pLoaderData->pResourceMgr != nullptr)
        {
            if (auto pAllocation = pLoaderData->pResourceMgr->FindAllocation(CacheId.c_str()))
            {
                const auto& TexDesc    = pAllocation->GetAtlas()->GetAtlasDesc();
                const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
                const auto  Size       = pAllocation->GetSize();

                gltf_image->width      = Size.x;
                gltf_image->height     = Size.y;
                gltf_image->component  = FmtAttribs.NumComponents;
                gltf_image->bits       = FmtAttribs.ComponentSize * 8;
                gltf_image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

                // Keep strong reference to ensure the allocation is alive (second time, but that's fine).
                pLoaderData->TexturesHold.emplace_back(std::move(pAllocation));

                return true;
            }
        }
        else if (pLoaderData->pTextureCache != nullptr)
        {
            auto& TexCache = *pLoaderData->pTextureCache;

            std::lock_guard<std::mutex> Lock{TexCache.TexturesMtx};

            auto it = TexCache.Textures.find(CacheId);
            if (it != TexCache.Textures.end())
            {
                if (auto pTexture = it->second.Lock())
                {
                    const auto& TexDesc    = pTexture->GetDesc();
                    const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);

                    gltf_image->width      = TexDesc.Width;
                    gltf_image->height     = TexDesc.Height;
                    gltf_image->component  = FmtAttribs.NumComponents;
                    gltf_image->bits       = FmtAttribs.ComponentSize * 8;
                    gltf_image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

                    // Keep strong reference to ensure the texture is alive (second time, but that's fine).
                    pLoaderData->TexturesHold.emplace_back(std::move(pTexture));

                    return true;
                }
                else
                {
                    // Texture is stale - remove it from the cache
                    TexCache.Textures.erase(it);
                }
            }
        }
    }

    VERIFY(size != 1, "The texture was previously cached, but was not found in the cache now");

    ImageLoadInfo LoadInfo;
    LoadInfo.Format = Image::GetFileFormat(image_data, size);
    if (LoadInfo.Format == IMAGE_FILE_FORMAT_UNKNOWN)
    {
        if (error != nullptr)
        {
            *error += FormatString("Unknown format for image[", gltf_image_idx, "] name = '", gltf_image->name, "'");
        }
        return false;
    }

    if (LoadInfo.Format == IMAGE_FILE_FORMAT_DDS || LoadInfo.Format == IMAGE_FILE_FORMAT_KTX)
    {
        // Store binary data directly
        gltf_image->image.resize(size);
        memcpy(gltf_image->image.data(), image_data, size);
        // Use pixel_type field to indicate the file format
        gltf_image->pixel_type = LoadInfo.Format;
    }
    else
    {
        auto pImageData = DataBlobImpl::Create(size);
        memcpy(pImageData->GetDataPtr(), image_data, size);
        RefCntAutoPtr<Image> pImage;
        Image::CreateFromDataBlob(pImageData, LoadInfo, &pImage);
        if (!pImage)
        {
            if (error != nullptr)
            {
                *error += FormatString("Failed to load image[", gltf_image_idx, "] name = '", gltf_image->name, "'");
            }
            return false;
        }
        const auto& ImgDesc = pImage->GetDesc();

        if (req_width > 0)
        {
            if (static_cast<Uint32>(req_width) != ImgDesc.Width)
            {
                if (error != nullptr)
                {
                    (*error) += FormatString("Image width mismatch for image[",
                                             gltf_image_idx, "] name = '", gltf_image->name,
                                             "': requested width: ",
                                             req_width, ", actual width: ",
                                             ImgDesc.Width);
                }
                return false;
            }
        }

        if (req_height > 0)
        {
            if (static_cast<Uint32>(req_height) != ImgDesc.Height)
            {
                if (error != nullptr)
                {
                    (*error) += FormatString("Image height mismatch for image[",
                                             gltf_image_idx, "] name = '", gltf_image->name,
                                             "': requested height: ",
                                             req_height, ", actual height: ",
                                             ImgDesc.Height);
                }
                return false;
            }
        }

        gltf_image->width      = ImgDesc.Width;
        gltf_image->height     = ImgDesc.Height;
        gltf_image->component  = 4;
        gltf_image->bits       = GetValueSize(ImgDesc.ComponentType) * 8;
        gltf_image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        size_t DstRowSize      = static_cast<size_t>(gltf_image->width) * gltf_image->component * (gltf_image->bits / 8);
        gltf_image->image.resize(static_cast<size_t>(gltf_image->height) * DstRowSize);
        auto*        pPixelsBlob = pImage->GetData();
        const Uint8* pSrcPixels  = static_cast<const Uint8*>(pPixelsBlob->GetDataPtr());
        if (ImgDesc.NumComponents == 3)
        {
            for (size_t row = 0; row < ImgDesc.Height; ++row)
            {
                for (size_t col = 0; col < ImgDesc.Width; ++col)
                {
                    Uint8*       DstPixel = gltf_image->image.data() + DstRowSize * row + col * gltf_image->component;
                    const Uint8* SrcPixel = pSrcPixels + ImgDesc.RowStride * row + col * ImgDesc.NumComponents;

                    DstPixel[0] = SrcPixel[0];
                    DstPixel[1] = SrcPixel[1];
                    DstPixel[2] = SrcPixel[2];
                    DstPixel[3] = 255;
                }
            }
        }
        else if (gltf_image->component == 4)
        {
            for (size_t row = 0; row < ImgDesc.Height; ++row)
            {
                memcpy(gltf_image->image.data() + DstRowSize * row, pSrcPixels + ImgDesc.RowStride * row, DstRowSize);
            }
        }
        else
        {
            *error += FormatString("Unexpected number of image components (", ImgDesc.NumComponents, ")");
            return false;
        }
    }

    return true;
}

bool FileExists(const std::string& abs_filename, void* user_data)
{
    // FileSystem::FileExists() is a pretty slow function.
    // Try to find the file in the cache first to avoid calling it.
    if (auto* pLoaderData = static_cast<LoaderData*>(user_data))
    {
        const auto CacheId = FileSystem::SimplifyPath(abs_filename.c_str());
        if (pLoaderData->pResourceMgr != nullptr)
        {
            if (pLoaderData->pResourceMgr->FindAllocation(CacheId.c_str()) != nullptr)
                return true;
        }
        else if (pLoaderData->pTextureCache != nullptr)
        {
            std::lock_guard<std::mutex> Lock{pLoaderData->pTextureCache->TexturesMtx};

            auto it = pLoaderData->pTextureCache->Textures.find(CacheId.c_str());
            if (it != pLoaderData->pTextureCache->Textures.end())
                return true;
        }

        if (pLoaderData->FileExists)
            return pLoaderData->FileExists(abs_filename.c_str());
    }

    return FileSystem::FileExists(abs_filename.c_str());
}

bool ReadWholeFile(std::vector<unsigned char>* out,
                   std::string*                err,
                   const std::string&          filepath,
                   void*                       user_data)
{
    VERIFY_EXPR(out != nullptr);
    VERIFY_EXPR(err != nullptr);

    // Try to find the file in the texture cache to avoid reading it
    if (auto* pLoaderData = static_cast<LoaderData*>(user_data))
    {
        const auto CacheId = FileSystem::SimplifyPath(filepath.c_str());
        if (pLoaderData->pResourceMgr != nullptr)
        {
            if (auto pAllocation = pLoaderData->pResourceMgr->FindAllocation(CacheId.c_str()))
            {
                // Keep strong reference to ensure the allocation is alive.
                pLoaderData->TexturesHold.emplace_back(std::move(pAllocation));
                // Tiny GLTF checks the size of 'out', it can't be empty
                out->resize(1);
                return true;
            }
        }
        else if (pLoaderData->pTextureCache != nullptr)
        {
            std::lock_guard<std::mutex> Lock{pLoaderData->pTextureCache->TexturesMtx};

            auto it = pLoaderData->pTextureCache->Textures.find(CacheId.c_str());
            if (it != pLoaderData->pTextureCache->Textures.end())
            {
                if (auto pTexture = it->second.Lock())
                {
                    // Keep strong reference to ensure the texture is alive.
                    pLoaderData->TexturesHold.emplace_back(std::move(pTexture));
                    // Tiny GLTF checks the size of 'out', it can't be empty
                    out->resize(1);
                    return true;
                }
            }
        }

        if (pLoaderData->ReadWholeFile)
            return pLoaderData->ReadWholeFile(filepath.c_str(), *out, *err);
    }

    FileWrapper pFile{filepath.c_str(), EFileAccessMode::Read};
    if (!pFile)
    {
        if (err)
        {
            (*err) += FormatString("Unable to open file ", filepath, "\n");
        }
        return false;
    }

    auto size = pFile->GetSize();
    if (size == 0)
    {
        if (err)
        {
            (*err) += FormatString("File is empty: ", filepath, "\n");
        }
        return false;
    }

    out->resize(size);
    pFile->Read(out->data(), size);

    return true;
}

} // namespace

} // namespace Callbacks

void Model::LoadFromFile(IRenderDevice*         pDevice,
                         IDeviceContext*        pContext,
                         const ModelCreateInfo& CI)
{
    if (CI.FileName == nullptr || *CI.FileName == 0)
        LOG_ERROR_AND_THROW("File path must not be empty");

    auto* const pTextureCache = CI.pTextureCache;
    auto* const pResourceMgr  = CI.pCacheInfo != nullptr ? CI.pCacheInfo->pResourceMgr : nullptr;
    if (CI.pTextureCache != nullptr && pResourceMgr != nullptr)
        LOG_WARNING_MESSAGE("Texture cache is ignored when resource manager is used");

    Callbacks::LoaderData LoaderData{pTextureCache, pResourceMgr, {}, ""};

    const std::string filename{CI.FileName};
    if (filename.find_last_of("/\\") != std::string::npos)
        LoaderData.BaseDir = filename.substr(0, filename.find_last_of("/\\"));
    LoaderData.BaseDir += '/';

    LoaderData.FileExists    = CI.FileExistsCallback;
    LoaderData.ReadWholeFile = CI.ReadWholeFileCallback;

    tinygltf::TinyGLTF gltf_context;
    gltf_context.SetImageLoader(Callbacks::LoadImageData, &LoaderData);
    tinygltf::FsCallbacks fsCallbacks = {};
    fsCallbacks.ExpandFilePath        = tinygltf::ExpandFilePath;
    fsCallbacks.FileExists            = Callbacks::FileExists;
    fsCallbacks.ReadWholeFile         = Callbacks::ReadWholeFile;
    fsCallbacks.WriteWholeFile        = tinygltf::WriteWholeFile;
    fsCallbacks.user_data             = &LoaderData;
    gltf_context.SetFsCallbacks(fsCallbacks);

    bool   binary = false;
    size_t extpos = filename.rfind('.', filename.length());
    if (extpos != std::string::npos)
    {
        binary = (filename.substr(extpos + 1, filename.length() - extpos) == "glb");
    }

    std::string     error;
    std::string     warning;
    tinygltf::Model gltf_model;

    bool fileLoaded = false;
    if (binary)
        fileLoaded = gltf_context.LoadBinaryFromFile(&gltf_model, &error, &warning, filename.c_str());
    else
        fileLoaded = gltf_context.LoadASCIIFromFile(&gltf_model, &error, &warning, filename.c_str());
    if (!fileLoaded)
    {
        LOG_ERROR_AND_THROW("Failed to load gltf file ", filename, ": ", error);
    }
    if (!warning.empty())
    {
        LOG_WARNING_MESSAGE("Loaded gltf file ", filename, " with the following warning:", warning);
    }

    LoadTextureSamplers(pDevice, gltf_model);
    LoadTextures(pDevice, gltf_model, LoaderData.BaseDir, pTextureCache, pResourceMgr);
    LoadMaterials(gltf_model, CI.MaterialLoadCallback);

    ModelBuilder Builder{CI, *this};

    if (!gltf_model.scenes.empty())
    {
        const tinygltf::Scene& scene = gltf_model.scenes[gltf_model.defaultScene >= 0 ? gltf_model.defaultScene : 0];
        for (auto node_idx : scene.nodes)
            Builder.LoadNode(TinyGltfModelWrapper{gltf_model}, nullptr, node_idx);
    }
    else
    {
        // Load all nodes if there is no scene
        for (int node_idx = 0; node_idx < static_cast<int>(gltf_model.nodes.size()); ++node_idx)
            Builder.LoadNode(TinyGltfModelWrapper{gltf_model}, nullptr, node_idx);
    }

    Builder.LoadAnimationAndSkin(TinyGltfModelWrapper{gltf_model});

    for (auto* node : LinearNodes)
    {
        // Assign skins
        if (node->SkinIndex >= 0)
        {
            node->pSkin = Skins[node->SkinIndex].get();
        }
    }

    // Initial pose
    for (auto& root_node : Nodes)
    {
        root_node->UpdateTransforms();
    }
    CalculateSceneDimensions();

    Extensions = gltf_model.extensionsUsed;

    Builder.InitBuffers(pDevice, pContext);

    if (pContext != nullptr)
    {
        PrepareGPUResources(pDevice, pContext);
    }
}

void Model::CalculateBoundingBox(Node* node, const Node* parent)
{
    BoundBox parentBvh = parent ? parent->BVH : BoundBox{dimensions.min, dimensions.max};

    if (node->pMesh)
    {
        if (node->pMesh->IsValidBB())
        {
            node->AABB = node->pMesh->BB.Transform(node->GetMatrix());
            if (node->Children.empty())
            {
                node->BVH.Min    = node->AABB.Min;
                node->BVH.Max    = node->AABB.Max;
                node->IsValidBVH = true;
            }
        }
    }

    parentBvh.Min = std::min(parentBvh.Min, node->BVH.Min);
    parentBvh.Max = std::max(parentBvh.Max, node->BVH.Max);

    for (auto& child : node->Children)
    {
        CalculateBoundingBox(child.get(), node);
    }
}

void Model::CalculateSceneDimensions()
{
    // Calculate binary volume hierarchy for all nodes in the scene
    for (auto* node : LinearNodes)
    {
        CalculateBoundingBox(node, nullptr);
    }

    dimensions.min = float3{+FLT_MAX, +FLT_MAX, +FLT_MAX};
    dimensions.max = float3{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (const auto* node : LinearNodes)
    {
        if (node->IsValidBVH)
        {
            dimensions.min = std::min(dimensions.min, node->BVH.Min);
            dimensions.max = std::max(dimensions.max, node->BVH.Max);
        }
    }

    // Calculate scene AABBTransform
    AABBTransform       = float4x4::Scale(dimensions.max[0] - dimensions.min[0], dimensions.max[1] - dimensions.min[1], dimensions.max[2] - dimensions.min[2]);
    AABBTransform[3][0] = dimensions.min[0];
    AABBTransform[3][1] = dimensions.min[1];
    AABBTransform[3][2] = dimensions.min[2];
}

void Model::UpdateAnimation(Uint32 index, float time)
{
    if (index > static_cast<Uint32>(Animations.size()) - 1)
    {
        LOG_WARNING_MESSAGE("No animation with index ", index);
        return;
    }
    Animation& animation = Animations[index];

    bool updated = false;
    for (auto& channel : animation.Channels)
    {
        AnimationSampler& sampler = animation.Samplers[channel.SamplerIndex];
        if (sampler.Inputs.size() > sampler.OutputsVec4.size())
        {
            continue;
        }

        for (size_t i = 0; i < sampler.Inputs.size() - 1; i++)
        {
            if ((time >= sampler.Inputs[i]) && (time <= sampler.Inputs[i + 1]))
            {
                float u = std::max(0.0f, time - sampler.Inputs[i]) / (sampler.Inputs[i + 1] - sampler.Inputs[i]);
                if (u <= 1.0f)
                {
                    switch (channel.PathType)
                    {
                        case AnimationChannel::PATH_TYPE::TRANSLATION:
                        {
                            float4 trans               = lerp(sampler.OutputsVec4[i], sampler.OutputsVec4[i + 1], u);
                            channel.pNode->Translation = float3(trans);
                            break;
                        }

                        case AnimationChannel::PATH_TYPE::SCALE:
                        {
                            float4 scale         = lerp(sampler.OutputsVec4[i], sampler.OutputsVec4[i + 1], u);
                            channel.pNode->Scale = float3(scale);
                            break;
                        }

                        case AnimationChannel::PATH_TYPE::ROTATION:
                        {
                            Quaternion q1;
                            q1.q.x = sampler.OutputsVec4[i].x;
                            q1.q.y = sampler.OutputsVec4[i].y;
                            q1.q.z = sampler.OutputsVec4[i].z;
                            q1.q.w = sampler.OutputsVec4[i].w;

                            Quaternion q2;
                            q2.q.x = sampler.OutputsVec4[i + 1].x;
                            q2.q.y = sampler.OutputsVec4[i + 1].y;
                            q2.q.z = sampler.OutputsVec4[i + 1].z;
                            q2.q.w = sampler.OutputsVec4[i + 1].w;

                            channel.pNode->Rotation = normalize(slerp(q1, q2, u));
                            break;
                        }

                        case AnimationChannel::PATH_TYPE::WEIGHTS:
                        {
                            UNEXPECTED("Weights are not currently supported");
                            break;
                        }
                    }
                    updated = true;
                }
            }
        }
    }

    if (updated)
    {
        for (auto& node : Nodes)
        {
            node->UpdateTransforms();
        }
    }
}

void Model::Transform(const float4x4& Matrix)
{
    for (auto& root_node : Nodes)
    {
        root_node->Matrix *= Matrix;
        root_node->UpdateTransforms();
    }

    CalculateSceneDimensions();
}

Node* Model::FindNode(Node* parent, Uint32 index)
{
    Node* nodeFound = nullptr;
    if (parent->OriginalIndex == index)
    {
        return parent;
    }
    for (auto& child : parent->Children)
    {
        nodeFound = FindNode(child.get(), index);
        if (nodeFound)
        {
            break;
        }
    }
    return nodeFound;
}


Node* Model::NodeFromIndex(uint32_t index)
{
    Node* nodeFound = nullptr;
    for (auto& node : Nodes)
    {
        nodeFound = FindNode(node.get(), index);
        if (nodeFound)
        {
            break;
        }
    }
    return nodeFound;
}


} // namespace GLTF

} // namespace Diligent
