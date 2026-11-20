// GLSL (desktop, any version the game uses) -> SPIR-V (glslang, relaxed Vulkan rules: loose uniforms are gathered
// into one block "mgl_Defaults") -> Metal Shading Language (SPIRV-Cross), with reflection of that block's members,
// the samplers and the vertex inputs, so the GL layer can place glUniform* values and bind textures and attributes.
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_msl.hpp>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include "shader.h"

static bool inited;
extern "C" int mgl_translate_linked(int stage, const char *src, MglShaderOut *out, const MglInput *prev, unsigned nprev)
{
    if (!inited) { glslang::InitializeProcess(); inited = true; }
    EShLanguage lang = stage == 0 ? EShLangVertex : stage == 1 ? EShLangFragment : EShLangGeometry;
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    sh.setEnvInputVulkanRulesRelaxed();
    sh.setGlobalUniformBlockName("mgl_Defaults");
    sh.setGlobalUniformSet(0); sh.setGlobalUniformBinding(0);
    sh.setAutoMapLocations(true); sh.setAutoMapBindings(true);
    EShMessages msg = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors);
    out->log[0] = 0;
    if (!sh.parse(GetDefaultResources(), 110, ECompatibilityProfile, false, false, msg)) {
        snprintf(out->log, sizeof out->log, "%s", sh.getInfoLog()); return 0;
    }
    glslang::TProgram prog; prog.addShader(&sh);
    if (!prog.link(msg) || !prog.mapIO()) { snprintf(out->log, sizeof out->log, "%s", prog.getInfoLog()); return 0; }
    std::vector<uint32_t> spv; glslang::GlslangToSpv(*prog.getIntermediate(lang), spv);
    try {
        spirv_cross::CompilerMSL msl(std::move(spv));
        auto active = msl.get_active_interface_variables();
        msl.set_enabled_interface_variables(std::move(active));    /* unused varyings out of the interface */
        if (prev && nprev) {
            auto r0 = msl.get_shader_resources(); uint32_t spare = 30;
            for (auto &a : r0.stage_inputs) {
                const std::string &n = msl.get_name(a.id); bool found = false;
                for (unsigned k = 0; k < nprev; k++) if (n == prev[k].name) { msl.set_decoration(a.id, spv::DecorationLocation, prev[k].location); found = true; break; }
                if (!found) msl.set_decoration(a.id, spv::DecorationLocation, spare--);
            }
        }
        spirv_cross::CompilerMSL::Options o; o.set_msl_version(2, 3); o.enable_decoration_binding = true;
        msl.set_msl_options(o);
        auto opts = msl.get_common_options(); opts.vertex.flip_vert_y = true; opts.vertex.fixup_clipspace = true; msl.set_common_options(opts);   /* GL row order kept (mgl.h) */
        std::string m = msl.compile();
        out->msl = strdup(m.c_str());
        // reflection: the default uniform block
        auto res = msl.get_shader_resources(); out->nuniforms = 0; out->block_size = 0;
        for (auto &u : res.uniform_buffers) {
            if (msl.get_name(u.base_type_id) != "mgl_Defaults" && msl.get_name(u.id) != "mgl_Defaults") continue;
            auto &t = msl.get_type(u.base_type_id);
            out->block_size = (unsigned)msl.get_declared_struct_size(t);
            for (unsigned k = 0; k < t.member_types.size() && out->nuniforms < MGL_MAX_UNIFORMS; k++) {
                MglUniform &x = out->uniforms[out->nuniforms++];
                snprintf(x.name, sizeof x.name, "%s", msl.get_member_name(u.base_type_id, k).c_str());
                x.offset = msl.type_struct_member_offset(t, k);
                auto &mt = msl.get_type(t.member_types[k]);
                x.columns = mt.columns; x.vecsize = mt.vecsize; x.basetype = (int)mt.basetype;
                x.array = mt.array.empty() ? 1 : mt.array[0];
                x.stride = mt.array.empty() ? 0 : msl.type_struct_member_array_stride(t, k);
            }
        }
        out->nsamplers = 0;
        for (auto &s : res.sampled_images) if (out->nsamplers < MGL_MAX_SAMPLERS) {
            MglSampler &x = out->samplers[out->nsamplers++];
            snprintf(x.name, sizeof x.name, "%s", msl.get_name(s.id).c_str());
            x.texture = x.sampler = msl.get_decoration(s.id, spv::DecorationBinding);   /* texture(n) and sampler(n) */
            auto &it = msl.get_type(s.base_type_id); x.dim = (int)it.image.dim; x.depth = it.image.depth; x.arrayed = it.image.arrayed;
        }
        out->ninputs = 0;
        if (lang == EShLangVertex) for (auto &a : res.stage_inputs) if (out->ninputs < MGL_MAX_INPUTS) {
            MglInput &x = out->inputs[out->ninputs++];
            snprintf(x.name, sizeof x.name, "%s", msl.get_name(a.id).c_str());
            x.location = msl.get_decoration(a.id, spv::DecorationLocation);
            auto &at = msl.get_type(a.base_type_id); x.vecsize = at.vecsize; x.basetype = (int)at.basetype;
        }
        out->noutputs = 0;
        if (lang == EShLangVertex) for (auto &a : res.stage_outputs) if (out->noutputs < 32) {
            MglInput &x = out->outputs[out->noutputs++];
            snprintf(x.name, sizeof x.name, "%s", msl.get_name(a.id).c_str());
            x.location = msl.get_decoration(a.id, spv::DecorationLocation);
        }
        out->uniform_buffer = 0;
        for (auto &u : res.uniform_buffers) if (msl.get_name(u.base_type_id) == "mgl_Defaults" || msl.get_name(u.id) == "mgl_Defaults")
            out->uniform_buffer = msl.get_decoration(u.id, spv::DecorationBinding);
    } catch (const std::exception &e) { snprintf(out->log, sizeof out->log, "SPIRV-Cross: %s", e.what()); return 0; }
    return 1;
}

extern "C" int mgl_translate(int stage, const char *src, MglShaderOut *out) { return mgl_translate_linked(stage, src, out, nullptr, 0); }
