// deko3d: SPIR-V to GLSL for uam with SPIRV-Cross (dk_glsl.h)
#include "dk_glsl.h"

#include <spirv_glsl.hpp>

namespace dkglsl {

namespace {

constexpr uint32_t kFirstTexture = 2;  // vk_shader.h kBindingTextures

}  // namespace

Glsl FromSpirv(const std::vector<uint32_t>& spirv) {
    Glsl out;
    try {
        spirv_cross::CompilerGLSL glsl(spirv);
        switch (glsl.get_execution_model()) {
            case spv::ExecutionModelVertex: out.stage = Stage::Vertex; break;
            case spv::ExecutionModelFragment: out.stage = Stage::Pixel; break;
            default: out.error = "neither a vertex nor a pixel shader"; return out;
        }
        spirv_cross::CompilerGLSL::Options opt;
        opt.version = 460;
        opt.es = false;
        opt.vulkan_semantics = false;   // uam's front end is desktop GLSL
        opt.separate_shader_objects = true;  // uam compiles each stage on its own: gl_PerVertex is redeclared
        opt.enable_420pack_extension = false;
        glsl.set_common_options(opt);

        // the HLSL's separate textures and samplers (t_i with s_i) as one combined sampler at the texture's slot
        glsl.build_combined_image_samplers();
        for (const spirv_cross::CombinedImageSampler& c : glsl.get_combined_image_samplers()) {
            uint32_t binding = glsl.get_decoration(c.image_id, spv::DecorationBinding);
            if (binding < kFirstTexture) {
                out.error = "texture outside the texture bindings";
                return out;
            }
            glsl.set_decoration(c.combined_id, spv::DecorationBinding, binding - kFirstTexture);
            glsl.set_name(c.combined_id, "tex" + std::to_string(binding - kFirstTexture));
        }
        // constant buffers keep their binding (0, 1); the descriptor set has no meaning in deko3d
        spirv_cross::ShaderResources res = glsl.get_shader_resources();
        for (const spirv_cross::Resource& r : res.uniform_buffers) glsl.unset_decoration(r.id, spv::DecorationDescriptorSet);
        // BREXTREME: gl_Position invariant, so two vertex shaders that compute a position the same way give the same
        // bits. The HLSL's precise only stops contractions inside one shader; uam still optimised each program on its
        // own, and a second pass over the same geometry with LESSEQUAL lost now and then (Bloody Roar Extreme's night
        // city: the lit base of a building showed through its dark layer for single frames on the console)
        if (out.stage == Stage::Vertex) {
            for (const spirv_cross::BuiltInResource& b : res.builtin_outputs) {
                if (b.builtin != spv::BuiltInPosition) continue;
                const spirv_cross::SPIRType& t = glsl.get_type(b.resource.base_type_id);
                if (glsl.has_decoration(t.self, spv::DecorationBlock)) {
                    for (uint32_t m = 0; m < uint32_t(t.member_types.size()); m++)
                        if (glsl.has_member_decoration(t.self, m, spv::DecorationBuiltIn) &&
                            glsl.get_member_decoration(t.self, m, spv::DecorationBuiltIn) == spv::BuiltInPosition)
                            glsl.set_member_decoration(t.self, m, spv::DecorationInvariant);
                } else {
                    glsl.set_decoration(b.resource.id, spv::DecorationInvariant);
                }
            }
        }
        out.text = glsl.compile();
    } catch (const std::exception& e) {
        out.error = e.what();
        out.text.clear();
    }
    return out;
}

}  // namespace dkglsl
