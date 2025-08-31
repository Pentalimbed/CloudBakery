#include <array>
#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>
#include <RE2/re2.h>

#include <d3dcompiler.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <DirectXMath.h>
#include <dxgi1_3.h>
#include <winrt/base.h>

using namespace std::literals;
using winrt::com_ptr;

// took from https://github.com/Microsoft/DirectXTK/wiki/throwIfFailed
namespace DX {
// Helper class for COM exceptions
class com_exception : public std::exception {
public:
    com_exception(HRESULT hr) :
        result(hr) {}

    const char* what() const noexcept override
    {
        static char s_str[64] = {};
        sprintf_s(s_str, "Failure with HRESULT of %08X",
                  static_cast<unsigned int>(result));
        return s_str;
    }

private:
    HRESULT result;
};

// Helper utility converts D3D API failures into exceptions.
inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) {
        throw com_exception(hr);
    }
}
} // namespace DX

// -----------------------------------------------------------------------------------------------------------------

struct Arguments {
    std::filesystem::path in_dir;
    std::filesystem::path out_dir;
    std::filesystem::path validation_dir;
};

struct Texture {
    winrt::com_ptr<ID3D11Texture2D>    tex = nullptr;
    com_ptr<ID3D11ShaderResourceView>  srv = nullptr;
    com_ptr<ID3D11UnorderedAccessView> uav = nullptr;
};

struct InputTexture {
    DirectX::XMFLOAT3                 light_direction = {1.F, 0.F, 0.F}; // note: unused for transmittance
    uint32_t                          width           = 0;
    uint32_t                          height          = 0;
    com_ptr<ID3D11ShaderResourceView> srv             = nullptr;
};

struct InputTexSet {
    uint32_t                  face; // see Common.hlsli
    InputTexture              tr;
    std::vector<InputTexture> colors;
};

struct BakeCBData {
    DirectX::XMFLOAT3 light_dir;
    float             weight;
    uint32_t          face;
    DirectX::XMFLOAT3 _pad;
};
static_assert(sizeof(BakeCBData) % 16 == 0);

struct D3dObjs {
    com_ptr<ID3D11Device1>        device  = nullptr;
    com_ptr<ID3D11DeviceContext1> context = nullptr;

    std::unordered_map<std::string, InputTexSet> tex_inputs;
    std::array<Texture, 3>                       tex_sh_coeffs = {};
    com_ptr<ID3D11Buffer>                        common_buffer = nullptr;
    com_ptr<ID3D11ComputeShader>                 bake_cs       = nullptr;
    com_ptr<ID3D11ComputeShader>                 validation_cs = nullptr;
};

namespace {
uint32_t faceStrToUint(const std::string& str)
{
    static const std::map<std::string, uint32_t> KEYMAP = {
        {"+x"s, 0},
        {"-x"s, 1},
        {"+y"s, 2},
        {"-y"s, 3},
        {"+z"s, 4},
    };
    return KEYMAP.at(str);
}

ID3D11ComputeShader* compileShader(ID3D11Device* device, const std::filesystem::path& path, const char* entry_point)
{
    spdlog::info("Compiling {} :{} ...", path.string(), entry_point);

    ID3DBlob* shader_blob   = nullptr;
    ID3DBlob* shader_errors = nullptr;

    if (!std::filesystem::exists(path)) {
        spdlog::error("Failed to compile shader: {} does not exist", path.string());
        return nullptr;
    }
    if (FAILED(D3DCompileFromFile(path.wstring().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                  entry_point, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader_blob, &shader_errors))) {
        spdlog::error("Shader compilation failed:\n\n{}", (shader_errors != nullptr) ? static_cast<char*>(shader_errors->GetBufferPointer()) : "Unknown error");
        return nullptr;
    }

    ID3D11ComputeShader* reg_shader = nullptr;
    if (FAILED(device->CreateComputeShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(), nullptr, &reg_shader))) {
        spdlog::error("Failed to create compute shader");
        return nullptr;
    }
    return reg_shader;
}

Texture initTex(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    Texture retval;

    D3D11_TEXTURE2D_DESC tex_desc = {
        .Width          = width,
        .Height         = height,
        .MipLevels      = 1,
        .ArraySize      = 1,
        .Format         = format,
        .SampleDesc     = {.Count = 1, .Quality = 0},
        .Usage          = D3D11_USAGE_DEFAULT,
        .BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        .CPUAccessFlags = 0,
        .MiscFlags      = 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format        = tex_desc.Format,
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D     = {.MostDetailedMip = 0, .MipLevels = 1}};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
        .Format        = tex_desc.Format,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
        .Texture2D     = {.MipSlice = 0}};


    DX::ThrowIfFailed(device->CreateTexture2D(&tex_desc, nullptr, retval.tex.put()));
    DX::ThrowIfFailed(device->CreateShaderResourceView(retval.tex.get(), &srv_desc, retval.srv.put()));
    DX::ThrowIfFailed(device->CreateUnorderedAccessView(retval.tex.get(), &uav_desc, retval.uav.put()));

    return retval;
}

HRESULT saveTextureToDDS(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* tex, const std::filesystem::path& out_path, bool compressed)
{
    // Capture texture into a ScratchImage
    DirectX::ScratchImage image;
    HRESULT               hr = DirectX::CaptureTexture(device, context, tex, image);
    if (FAILED(hr)) {
        spdlog::error("Failed to capture texture {}\n");
        return hr;
    }

    // Compress to BC6H format
    DirectX::ScratchImage compressed_image;
    if (compressed) {
        hr = DirectX::Compress(
            image.GetImages(),
            image.GetImageCount(),
            image.GetMetadata(),
            DXGI_FORMAT_BC6H_SF16, // BC6H format
            DirectX::TEX_COMPRESS_DEFAULT,
            1.0F,
            compressed_image);

        if (FAILED(hr)) {
            spdlog::error("Failed to compress texture to BC6H");
            return hr;
        }
    }

    auto& target_image = compressed ? compressed_image : image;

    // Save to DDS file
    hr = DirectX::SaveToDDSFile(
        target_image.GetImages(),
        target_image.GetImageCount(),
        target_image.GetMetadata(),
        DirectX::DDS_FLAGS_NONE,
        out_path.wstring().c_str());

    if (FAILED(hr)) {
        spdlog::error("Failed to save DDS file");
        return hr;
    }

    return S_OK;
}
} // namespace


int main(int argc, char* argv[])
{
    Arguments args;
    D3dObjs   d3d{};

    // Arg parse
    {
        argparse::ArgumentParser program("cloud-bakery");
        program.add_argument("-i", "--input-dir")
            .help("Input directory of all dds files.\n"
                  "Valid dds file names are \"(any_identifier)_(+/-)(x/y/z)_(direction_x)_(direction_y)_(direction_z).dds\".")
            .default_value("./input"s);
        program.add_argument("-o", "--output-dir")
            .help("Output directory of baked SH.\n"
                  "The output dds will be named as \"(identifier)_(+/-)(x/y/z)_sh(0/1/2).dds\" where the identifier matches the input.")
            .default_value("./output"s);
        program.add_argument("-v", "--validation-dir")
            .help("Output directory of reconstructed images.\n"
                  "Specifying this to generate a reconstructed image of the first image of the set")
            .default_value(std::string{});

        try {
            program.parse_args(argc, argv);
        } catch (const std::exception& err) {
            spdlog::error("Error while parsing arguments:\n"
                          "{}",
                          err.what());
            return E_INVALIDARG;
        }

        args.in_dir         = program.get("-i");
        args.out_dir        = program.get("-o");
        args.validation_dir = program.get("-v");

        if (!(std::filesystem::exists(args.in_dir) && std::filesystem::is_directory(args.in_dir))) {
            spdlog::error("Invalid input directory: {}", args.in_dir.string());
            return E_FAIL;
        }

        if (std::filesystem::exists(args.out_dir) && !std::filesystem::is_directory(args.out_dir)) {
            spdlog::error("Output directory exists and is not a folder: {}", args.in_dir.string());
            return E_FAIL;
        }

        if (std::filesystem::exists(args.validation_dir) && !std::filesystem::is_directory(args.validation_dir)) {
            spdlog::error("Validation directory exists and is not a folder: {}", args.in_dir.string());
            return E_FAIL;
        }

        if (!std::filesystem::exists(args.out_dir))
            std::filesystem::create_directory(args.out_dir);

        if (!args.validation_dir.empty() && !std::filesystem::exists(args.validation_dir))
            std::filesystem::create_directory(args.validation_dir);
    }

    // Initialize d3d device & context
    {
        ID3D11Device*        base_device      = nullptr;
        ID3D11DeviceContext* base_device_ctxt = nullptr;
        D3D_FEATURE_LEVEL    feat_lvls[]      = {D3D_FEATURE_LEVEL_11_0};
        UINT                 creation_flags   = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, creation_flags,
                                       feat_lvls, ARRAYSIZE(feat_lvls),
                                       D3D11_SDK_VERSION, &base_device,
                                       nullptr, &base_device_ctxt);
        if (FAILED(hr)) {
            spdlog::error("Failed to create ID3D11Device");
            return hr;
        }

        // Get 1.1 interface of D3D11 Device and Context
        hr = base_device->QueryInterface(IID_PPV_ARGS(&d3d.device));
        if (FAILED(hr)) {
            spdlog::error("Failed to create ID3D11Device1");
            return hr;
        }
        base_device->Release();

        hr = base_device_ctxt->QueryInterface(IID_PPV_ARGS(&d3d.context));
        if (FAILED(hr)) {
            spdlog::error("Failed to create ID3D11DeviceContext1");
            return hr;
        }
        base_device_ctxt->Release();
    }

    // Read textures
    {
        const RE2 tr_file_re{R"(^(.*)_([+-][xyz])_tr.dds$)"};
        const RE2 color_file_re{R"(^(.*)_([+-][xyz])_([+-]?(?:\d*\.\d+|\d+\.\d*|\d+))_([+-]?(?:\d*\.\d+|\d+\.\d*|\d+))_([+-]?(?:\d*\.\d+|\d+\.\d*|\d+)).dds$)"};
        for (auto const& dir_entry : std::filesystem::directory_iterator{args.in_dir}) {
            auto filename_path = dir_entry.path().filename();
            auto filename      = filename_path.string();
            if (filename_path.extension() != ".dds") {
                spdlog::info("Skipping {}", filename);
                continue;
            }
            spdlog::info("Reading {} ...", filename);

            std::string  identifier;
            std::string  face_str;
            InputTexture tex;

            bool is_tr = RE2::FullMatch(filename, tr_file_re, &identifier, &face_str);
            if (!is_tr && !RE2::FullMatch(filename, color_file_re, &identifier, &face_str, &tex.light_direction.x, &tex.light_direction.y, &tex.light_direction.z)) {
                spdlog::warn("\t{} does not match the naming pattern.", filename);
                continue;
            }

            ID3D11Resource* temp_rsrc = nullptr;
            auto            hr        = DirectX::CreateDDSTextureFromFile(d3d.device.get(), dir_entry.path().wstring().c_str(), &temp_rsrc, tex.srv.put());
            if (FAILED(hr)) {
                spdlog::warn("\tFailed to read texture from {}", filename);
                continue;
            }

            ID3D11Texture2D* temp_tex = nullptr;
            hr                        = temp_rsrc->QueryInterface(IID_PPV_ARGS(&temp_tex));
            if (FAILED(hr)) {
                spdlog::warn("\t{} is not a 2d texture", filename);
                continue;
            }

            D3D11_TEXTURE2D_DESC tex_desc;
            temp_tex->GetDesc(&tex_desc);
            tex.width  = tex_desc.Width;
            tex.height = tex_desc.Height;

            std::string key = std::format("{}_{}", identifier, face_str);
            if (!d3d.tex_inputs.contains(key))
                d3d.tex_inputs[key] = {};

            if (is_tr) {
                d3d.tex_inputs[key].tr = tex;
            } else {
                DirectX::XMStoreFloat3(&tex.light_direction, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&tex.light_direction)));
                d3d.tex_inputs[key].colors.push_back(tex);
            }
            d3d.tex_inputs[key].face = faceStrToUint(face_str);

            spdlog::info("\tLoaded {} ({} x {})", filename, tex.width, tex.height);
        }
    }

    // Initialize other d3d structures
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth      = (sizeof(BakeCBData) + (64 - 1)) & ~(64 - 1),
            .Usage          = D3D11_USAGE_DEFAULT,
            .BindFlags      = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = 0,
        };
        auto hr = d3d.device->CreateBuffer(&desc, nullptr, d3d.common_buffer.put());
        if (FAILED(hr)) {
            spdlog::warn("Failed to create constant buffer");
            return hr;
        }
    }

    {
        auto* base_cs = compileShader(d3d.device.get(), "./shaders/Bake.cs.hlsl", "main");
        if (base_cs == nullptr)
            return E_FAIL;
        d3d.bake_cs.attach(base_cs);
    }

    {
        auto* base_cs = compileShader(d3d.device.get(), "./shaders/Validation.cs.hlsl", "main");
        if (base_cs == nullptr)
            return E_FAIL;
        d3d.validation_cs.attach(base_cs);
    }

    // Common setup
    {
        auto* cb = d3d.common_buffer.get();
        d3d.context->CSSetConstantBuffers(0, 1, &cb);
        d3d.context->CSSetShader(d3d.bake_cs.get(), nullptr, 0);
    }

    // Process
    for (auto const& [key, tex_set] : d3d.tex_inputs) {
        spdlog::info("Processing texture set {} ...", key);

        if (tex_set.tr.srv == nullptr) {
            spdlog::warn("\tTexture set {} has no transmittance texture ({}_tr.dds). Skipping the whole set", key, key);
            continue;
        }

        const auto width  = tex_set.tr.width;
        const auto height = tex_set.tr.height;

        // Checking equal dimensions
        if (std::ranges::any_of(tex_set.colors, [width, height](auto const& tex) { return (tex.width != width) || (tex.height != height); })) {
            spdlog::warn("\tTexture set {} has more than two sizes. Skipping the whole set", key);
            continue;
        }

        for (auto& tex_sh_coeff : d3d.tex_sh_coeffs) {
            tex_sh_coeff    = initTex(d3d.device.get(), width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
            float values[4] = {0, 0, 0, 0};
            d3d.context->ClearUnorderedAccessViewFloat(tex_sh_coeff.uav.get(), values);
        }
        d3d.context->CSSetShader(d3d.bake_cs.get(), nullptr, 0);

        BakeCBData cb_data{
            .weight = 1.F / static_cast<float>(tex_set.colors.size()),
            .face   = tex_set.face,
        };

        // Dispatch
        for (auto const& entry : tex_set.colors) {
            cb_data.light_dir = entry.light_direction,
            d3d.context->UpdateSubresource(d3d.common_buffer.get(), 0, nullptr, &cb_data, 0, 0);

            auto srvs = std::array{
                entry.srv.get(),
                tex_set.tr.srv.get(),
            };
            d3d.context->CSSetShaderResources(0, srvs.size(), srvs.data());

            auto uavs = std::array{d3d.tex_sh_coeffs[0].uav.get(), d3d.tex_sh_coeffs[1].uav.get(), d3d.tex_sh_coeffs[2].uav.get()};
            d3d.context->CSSetUnorderedAccessViews(0, uavs.size(), uavs.data(), nullptr);

            d3d.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            // clear
            srvs.fill(nullptr);
            d3d.context->CSSetShaderResources(0, srvs.size(), srvs.data());
            uavs.fill(nullptr);
            d3d.context->CSSetUnorderedAccessViews(0, uavs.size(), uavs.data(), nullptr);
        }

        // Save textures
        for (size_t i = 0; i < d3d.tex_sh_coeffs.size(); i++)
            DX::ThrowIfFailed(saveTextureToDDS(d3d.device.get(), d3d.context.get(), d3d.tex_sh_coeffs[i].tex.get(),
                                               args.out_dir / std::format("{}_sh{}.dds", key, i), true));

        // Validation
        if (!args.validation_dir.empty()) {
            auto  valid_tex = initTex(d3d.device.get(), width, height, DXGI_FORMAT_R32_FLOAT);
            auto* uav       = valid_tex.uav.get();
            d3d.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

            auto srvs = std::array{
                d3d.tex_sh_coeffs[0].srv.get(),
                d3d.tex_sh_coeffs[1].srv.get(),
                d3d.tex_sh_coeffs[2].srv.get(),
                tex_set.tr.srv.get(),
            };
            d3d.context->CSSetShaderResources(0, srvs.size(), srvs.data());

            d3d.context->CSSetShader(d3d.validation_cs.get(), nullptr, 0);
            d3d.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            // clear
            uav = nullptr;
            d3d.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            srvs.fill(nullptr);
            d3d.context->CSSetShaderResources(0, srvs.size(), srvs.data());

            // save
            DX::ThrowIfFailed(saveTextureToDDS(d3d.device.get(), d3d.context.get(), valid_tex.tex.get(),
                                               args.validation_dir / std::format("{}_{:.2f}_{:.2f}_{:.2f}_re.dds", key, cb_data.light_dir.x, cb_data.light_dir.y, cb_data.light_dir.z), false));
        }

        spdlog::info("\tDone");
    }


    return S_OK;
}
