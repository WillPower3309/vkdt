MOD_LDFLAGS=-lm
RT_DEPS=pipe/modules/rt/material.glsl pipe/modules/rt/colour.glsl pipe/modules/rt/fresnel.glsl pipe/modules/shared/render3d.glsl pipe/modules/rt/env.glsl pipe/modules/rt/pathtrace.glsl
pipe/modules/rt/main.comp.spv: $(RT_DEPS)
pipe/modules/rt/gbuf.comp.spv: $(RT_DEPS)
