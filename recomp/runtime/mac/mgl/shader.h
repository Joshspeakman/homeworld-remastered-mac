/* GLSL -> MSL translation for the Metal GL backend (shader.cpp) */
#ifndef MGL_SHADER_H
#define MGL_SHADER_H
#ifdef __cplusplus
extern "C" {
#endif
#define MGL_MAX_UNIFORMS 128
#define MGL_MAX_SAMPLERS 16
#define MGL_MAX_INPUTS 16
typedef struct MglUniform { char name[64]; unsigned offset, columns, vecsize, array, stride; int basetype; } MglUniform;
typedef struct MglSampler { char name[64]; unsigned texture, sampler; int dim, depth, arrayed; } MglSampler;   /* dim: 1 = 2D, 3 = cube (spv::Dim) */
typedef struct MglInput { char name[64]; unsigned location, vecsize; int basetype; } MglInput;
typedef struct MglShaderOut {
    char *msl; char log[4096];
    unsigned block_size, uniform_buffer, nuniforms, nsamplers, ninputs;
    MglUniform uniforms[MGL_MAX_UNIFORMS]; MglSampler samplers[MGL_MAX_SAMPLERS]; MglInput inputs[MGL_MAX_INPUTS];
    unsigned noutputs; MglInput outputs[32];          /* vertex stage outputs (varyings) by name */
} MglShaderOut;
int mgl_translate(int stage, const char *glsl, MglShaderOut *out);   /* stage 0 vertex, 1 fragment, 2 geometry */
/* the same, with the stage inputs placed at the locations of the previous stage's outputs of the same name (GL
 * matches varyings by name; glslang numbers them in declaration order) */
int mgl_translate_linked(int stage, const char *glsl, MglShaderOut *out, const MglInput *prev_outputs, unsigned nprev);
#ifdef __cplusplus
}
#endif
#endif
