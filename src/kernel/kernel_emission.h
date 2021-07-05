/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* Direction Emission */
ccl_device_noinline_cpu float3 direct_emissive_eval(KernelGlobals *kg,
                                                    ShaderData *emission_sd,
                                                    LightSample *ls,
                                                    ccl_addr_space PathState *state,
                                                    float3 I,
                                                    differential3 dI,
                                                    float t,
                                                    float time)
{
  /* setup shading at emitter */
  float3 eval = zero_float3();

  if (shader_constant_emission_eval(kg, ls->shader, &eval)) {
    if ((ls->prim != PRIM_NONE) && dot(ls->Ng, I) < 0.0f) {
      ls->Ng = -ls->Ng;
    }
  }
  else {
    /* Setup shader data and call shader_eval_surface once, better
     * for GPU coherence and compile times. */
#ifdef __BACKGROUND_MIS__
    if (ls->type == LIGHT_BACKGROUND) {
      Ray ray;
      ray.D = ls->D;
      ray.P = ls->P;
      ray.t = 1.0f;
      ray.time = time;
      ray.dP = differential3_zero();
      ray.dD = dI;

      shader_setup_from_background(kg, emission_sd, &ray);
    }
    else
#endif
    {
      shader_setup_from_sample(kg,
                               emission_sd,
                               ls->P,
                               ls->Ng,
                               I,
                               &dI,
                               ls->shader,
                               ls->object,
                               ls->prim,
                               ls->u,
                               ls->v,
                               t,
                               time,
                               false,
                               ls->lamp);

      ls->Ng = emission_sd->Ng;
    }

    /* No proper path flag, we're evaluating this for all closures. that's
     * weak but we'd have to do multiple evaluations otherwise. */
    path_state_modify_bounce(state, true);
    shader_eval_surface(kg, emission_sd, state, NULL, PATH_RAY_EMISSION);
    path_state_modify_bounce(state, false);

    /* Evaluate closures. */
#ifdef __BACKGROUND_MIS__
    if (ls->type == LIGHT_BACKGROUND) {
      eval = shader_background_eval(emission_sd);
    }
    else
#endif
    {
      eval = shader_emissive_eval(emission_sd);
    }
  }

  eval *= ls->eval_fac;

  if (ls->lamp != LAMP_NONE) {
    const ccl_global KernelLight *klight = &kernel_tex_fetch(__lights, ls->lamp);
    eval *= make_float3(klight->strength[0], klight->strength[1], klight->strength[2]);
  }

  return eval;
}

ccl_device_noinline_cpu bool direct_emission(KernelGlobals *kg,
                                             ShaderData *sd,
                                             ShaderData *emission_sd,
                                             LightSample *ls,
                                             ccl_addr_space PathState *state,
                                             Ray *ray,
                                             BsdfEval *eval,
                                             bool *is_lamp,
                                             float rand_terminate)
{
  if (ls->pdf == 0.0f)
    return false;

  differential3 dD;
  differential3 dN;
#ifdef __DNDU__
  dN.dx = sd->dNdx;
  dN.dy = sd->dNdy;
#else
  dN = differential3_zero();
#endif
  /* This is how differentials are calculated for a perfect specular reflection.
   * This is not the exact value that we should be getting here,
   * but it's still better than using zero differentials. */
  differential_reflect(&dD, sd->I, &sd->dI, sd->N, &dN);

  /* evaluate closure */
  emission_sd->dP = sd->dP;

  float3 light_eval = direct_emissive_eval(
      kg, emission_sd, ls, state, -ls->D, dD, ls->t, sd->time);

  if (is_zero(light_eval))
    return false;

    /* evaluate BSDF at shading point */

#ifdef __VOLUME__
  if (sd->prim != PRIM_NONE)
    shader_bsdf_eval(kg, sd, ls->D, eval, ls->pdf, ls->shader & SHADER_USE_MIS);
  else {
    float bsdf_pdf;
    shader_volume_phase_eval(kg, sd, ls->D, eval, &bsdf_pdf);
    if (ls->shader & SHADER_USE_MIS) {
      /* Multiple importance sampling. */
      float mis_weight = power_heuristic(ls->pdf, bsdf_pdf);
      light_eval *= mis_weight;
    }
  }
#else
  shader_bsdf_eval(kg, sd, ls->D, eval, ls->pdf, ls->shader & SHADER_USE_MIS);
#endif

  bsdf_eval_mul3(eval, light_eval / ls->pdf);

#ifdef __PASSES__
  /* use visibility flag to skip lights */
  if (ls->shader & SHADER_EXCLUDE_ANY) {
    if (ls->shader & SHADER_EXCLUDE_DIFFUSE)
      eval->diffuse = zero_float3();
    if (ls->shader & SHADER_EXCLUDE_GLOSSY)
      eval->glossy = zero_float3();
    if (ls->shader & SHADER_EXCLUDE_TRANSMIT)
      eval->transmission = zero_float3();
    if (ls->shader & SHADER_EXCLUDE_SCATTER)
      eval->volume = zero_float3();
  }
#endif

  if (bsdf_eval_is_zero(eval))
    return false;

  if (kernel_data.integrator.light_inv_rr_threshold > 0.0f
#ifdef __SHADOW_TRICKS__
      && (state->flag & PATH_RAY_SHADOW_CATCHER) == 0
#endif
  ) {
    float probability = max3(fabs(bsdf_eval_sum(eval))) *
                        kernel_data.integrator.light_inv_rr_threshold;
    if (probability < 1.0f) {
      if (rand_terminate >= probability) {
        return false;
      }
      bsdf_eval_mul(eval, 1.0f / probability);
    }
  }

  if (ls->shader & SHADER_CAST_SHADOW) {
    /* setup ray */
    bool transmit = (dot(sd->Ng, ls->D) < 0.0f);
    ray->P = ray_offset(sd->P, (transmit) ? -sd->Ng : sd->Ng);

    if (ls->t == FLT_MAX) {
      /* distant light */
      ray->D = ls->D;
      ray->t = ls->t;
    }
    else {
      /* other lights, avoid self-intersection */
      ray->D = ray_offset(ls->P, ls->Ng) - ray->P;
      ray->D = normalize_len(ray->D, &ray->t);
    }

    ray->dP = sd->dP;
    ray->dD = differential3_zero();
  }
  else {
    /* signal to not cast shadow ray */
    ray->t = 0.0f;
  }

  /* return if it's a lamp for shadow pass */
  *is_lamp = (ls->prim == PRIM_NONE && ls->type != LIGHT_BACKGROUND);

  return true;
}

/* Indirect Primitive Emission */

ccl_device_noinline_cpu float3 indirect_primitive_emission(
    KernelGlobals *kg, ShaderData *sd, float t, int path_flag, float bsdf_pdf)
{
  /* evaluate emissive closure */
  float3 L = shader_emissive_eval(sd);

#ifdef __HAIR__
  if (!(path_flag & PATH_RAY_MIS_SKIP) && (sd->flag & SD_USE_MIS) &&
      (sd->type & PRIMITIVE_ALL_TRIANGLE))
#else
  if (!(path_flag & PATH_RAY_MIS_SKIP) && (sd->flag & SD_USE_MIS))
#endif
  {
    /* multiple importance sampling, get triangle light pdf,
     * and compute weight with respect to BSDF pdf */
    float pdf = triangle_light_pdf(kg, sd, t);
    pdf *= light_distribution_pdf(kg, sd->P_pick, sd->V_pick, sd->t_pick, sd->prim, sd->object);
    float mis_weight = power_heuristic(bsdf_pdf, pdf);

    return L * mis_weight;
  }

  return L;
}

/* Indirect Lamp Emission */

ccl_device_noinline_cpu void indirect_lamp_emission(KernelGlobals *kg,
                                                    ShaderData *emission_sd,
                                                    ccl_addr_space PathState *state,
                                                    ccl_global float *buffer,
                                                    PathRadiance *L,
                                                    Ray *ray,
                                                    float3 throughput)
{
  for (int lamp = 0; lamp < kernel_data.integrator.num_all_lights; lamp++) {
    LightSample ls ccl_optional_struct_init;

    if (!lamp_light_eval(kg, lamp, ray->P, ray->D, ray->t, &ls))
      continue;

#ifdef __PASSES__
    /* use visibility flag to skip lights */
    if (ls.shader & SHADER_EXCLUDE_ANY) {
      if (((ls.shader & SHADER_EXCLUDE_DIFFUSE) && (state->flag & PATH_RAY_DIFFUSE)) ||
          ((ls.shader & SHADER_EXCLUDE_GLOSSY) &&
           ((state->flag & (PATH_RAY_GLOSSY | PATH_RAY_REFLECT)) ==
            (PATH_RAY_GLOSSY | PATH_RAY_REFLECT))) ||
          ((ls.shader & SHADER_EXCLUDE_TRANSMIT) && (state->flag & PATH_RAY_TRANSMIT)) ||
          ((ls.shader & SHADER_EXCLUDE_SCATTER) && (state->flag & PATH_RAY_VOLUME_SCATTER)))
        continue;
    }
#endif

    float3 lamp_L = direct_emissive_eval(
        kg, emission_sd, &ls, state, -ray->D, ray->dD, ls.t, ray->time);

#ifdef __VOLUME__
    if (state->volume_stack[0].shader != SHADER_NONE) {
      /* shadow attenuation */
      Ray volume_ray = *ray;
      volume_ray.t = ls.t;
      float3 volume_tp = one_float3();
      kernel_volume_shadow(kg, emission_sd, state, &volume_ray, &volume_tp);
      lamp_L *= volume_tp;
    }

#endif

    if (!(state->flag & PATH_RAY_MIS_SKIP)) {
      /* multiple importance sampling, get regular light pdf,
       * and compute weight with respect to BSDF pdf */


      /* multiply with light picking probablity to pdf */
      ls.pdf *= light_distribution_pdf(
          kg, emission_sd->P_pick, emission_sd->V_pick, emission_sd->t_pick, ~ls.lamp, -1);
      float mis_weight = power_heuristic(state->ray_pdf, ls.pdf);
      lamp_L *= mis_weight;
    }

    path_radiance_accum_emission(kg, L, state, buffer, throughput, lamp_L, ls.group);
  }
}

#define LIGHT_TREE_MAX_DEPTH 12
#define LIGHT_TREE_BVH_STACK_SIZE (1 + 4 * LIGHT_TREE_MAX_DEPTH + 3)

ccl_device_forceinline int intersect_ray_aabb(
  const float3 P,
  const float3 inv_D,
  const float t,
  const float3 bb_min0,
  const float3 bb_max0,
  float* t_out
  ) {
  const float3 c0lo = (bb_min0 - P) * inv_D;
  const float3 c0hi = (bb_max0 - P) * inv_D;
  const float c0min = max4(0.0, min(c0lo.x, c0hi.x), min(c0lo.y, c0hi.y), min(c0lo.z, c0hi.z));
  const float c0max = min4(t, max(c0lo.x, c0hi.x), max(c0lo.y, c0hi.y), max(c0lo.z, c0hi.z));
  *t_out = c0min;
  return (c0max >= c0min) ? 1 : 0;
}

/* Traverse the scene and find the intersection with any light */
ccl_device_noinline_cpu void indirect_lamp_emission_light_tree(KernelGlobals *kg,
                                                        ShaderData *emission_sd,
                                                        ccl_addr_space PathState *state,
                                                        ccl_global float *buffer,
                                                        PathRadiance *L,
                                                        Ray *ray,
                                                        float3 throughput)
{
    /* Precompute values for intersection */
    float3 D_inv = rcp(ray->D); // todo: safe

    /* Initialize traversal stack */
    int stack[LIGHT_TREE_BVH_STACK_SIZE];
    int stack_ptr = 0;
    stack[stack_ptr] = 0; /* root */

    KernelLightTreeNode node, childl, childr;

    while (true) pop:
    {
      if (stack_ptr < 0) {
        break;
      }

      /* Pop the node */
      int cur = stack[stack_ptr];
      --stack_ptr;

      /* Keep traversing children  */
      while (true) {
        /* Fetch current node and check if its interior */        
        node = kernel_tex_fetch(__light_tree_nodes, cur);
        if (node.right_child_offset == -1) {
          break;
        }

        /* Calculate intersection with children */
        childl = kernel_tex_fetch(__light_tree_nodes, node.right_child_offset);
        childr = kernel_tex_fetch(__light_tree_nodes, node.right_child_offset + 1);

        bbminl = make_float3(childl.bbox_min.x, childl.bbox_min.y, childl.bbox_min.z);
        bbmaxl = make_float3(childl.bbox_max.x, childl.bbox_max.y, childl.bbox_max.z);
        bbminr = make_float3(childr.bbox_min.x, childr.bbox_min.y, childr.bbox_min.z);
        bbmaxr = make_float3(childr.bbox_max.x, childr.bbox_max.y, childr.bbox_max.z);

        float intersect_t[2];
        int intersect = intersect_ray_aabb(ray->P, D_inv, ray->t, bbminl, bbmaxl, intersect_t);
        intersect |= (intersect_ray_aabb(ray->P, D_inv, ray->t, bbminr, bbmaxr, intersect_t + 1) ? 2 : 0);

        if (intersect == 3) {
          ++stack_ptr;
          kernel_assert(stack_ptr < LIGHT_TREE_BVH_STACK_SIZE);

          /* Both intersected, choose closest and push the other */
          if (intersect[0] < intersect[1]) {
            cur = node.right_child_offset;
            stack[stack_ptr] = node.right_child_offset + 1;
          } else {
            cur = node.right_child_offset + 1;
            stack[stack_ptr] = node.right_child_offset;
          }
        } else if (intersect == 2) {
          cur = node.right_child_offset + 1;
        } else if (intersect == 1) {
          cur = node.right_child_offset;
        } else {
          /* Go to the next item in the stack */
          goto pop;
        }
      }

      /* Processing a leaf node */
      kernel_assert(node.right_child_offset == -1);

      /* Find the range of emitters for this leaf */
      int emitters_start = kernel_tex_fetch(__leaf_to_first_emitter, cur);
      int emitters_end = emitters_start + node.num_lights;

      for (int i = emitters_start; i < emitters_end; ++i) {
        /* Fetch the bounding box */
        KernelLightTreeLeaf leaf = kernel_tex_fetch(__light_tree_leaf_emitters, i);

        float intersect_t;
        if (intersect_ray_aabb(ray->P, D_inv, ray->t, leaf.bbox_min, leaf.bbox_max, &intersect_t)) {
          /* It's time to look up the real light */
          int light_idx = kernel_tex_fetch(__light_tree_emitter_to_light);

          /* We skip emitters which correspond to emissive triangles
           * as they are already intersected in the geometry bvh */
          if (light_idx < 0) {
            continue;
          }

          KernelLight light = kernel_tex_fetch(__lights, -light_idx - 1);
          LightType type = (LightType)light.type
          if (type == LIGHT_DISTANT) {
            // float3 lightD = make_float3(klight->co[0], klight->co[1], klight->co[2]);
            // float costheta = dot(-lightD, D);
            // float cosangle = klight->distant.cosangle;

            if (costheta < cosangle) {
              continue;
            }

            /* intersection successful */
          } else if (type == LIGHT_POINT || type == LIGHT_SPOT) {


          } else if (type == LIGHT_AREA) {

          }
        }
      }

      break;
    }
}

/* Indirect Background */

ccl_device_noinline_cpu float3 indirect_background(KernelGlobals *kg,
                                                   ShaderData *emission_sd,
                                                   ccl_addr_space PathState *state,
                                                   ccl_global float *buffer,
                                                   ccl_addr_space Ray *ray)
{
#ifdef __BACKGROUND__
  int shader = kernel_data.background.surface_shader;

  /* Use visibility flag to skip lights. */
  if (shader & SHADER_EXCLUDE_ANY) {
    if (((shader & SHADER_EXCLUDE_DIFFUSE) && (state->flag & PATH_RAY_DIFFUSE)) ||
        ((shader & SHADER_EXCLUDE_GLOSSY) &&
         ((state->flag & (PATH_RAY_GLOSSY | PATH_RAY_REFLECT)) ==
          (PATH_RAY_GLOSSY | PATH_RAY_REFLECT))) ||
        ((shader & SHADER_EXCLUDE_TRANSMIT) && (state->flag & PATH_RAY_TRANSMIT)) ||
        ((shader & SHADER_EXCLUDE_CAMERA) && (state->flag & PATH_RAY_CAMERA)) ||
        ((shader & SHADER_EXCLUDE_SCATTER) && (state->flag & PATH_RAY_VOLUME_SCATTER)))
      return zero_float3();
  }

  /* Evaluate background shader. */
  float3 L = zero_float3();
  if (!shader_constant_emission_eval(kg, shader, &L)) {
#  ifdef __SPLIT_KERNEL__
    Ray priv_ray = *ray;
    shader_setup_from_background(kg, emission_sd, &priv_ray);
#  else
    shader_setup_from_background(kg, emission_sd, ray);
#  endif

    path_state_modify_bounce(state, true);
    shader_eval_surface(kg, emission_sd, state, buffer, state->flag | PATH_RAY_EMISSION);
    path_state_modify_bounce(state, false);

    L = shader_background_eval(emission_sd);
  }

  /* Background MIS weights. */
#  ifdef __BACKGROUND_MIS__
  /* Check if background light exists or if we should skip pdf. */

  /* consider shading point at previous non-transparent bounce */
  float3 P_pick = ray->P - state->ray_t * ray->D;

  if (!(state->flag & PATH_RAY_MIS_SKIP) && kernel_data.background.use_mis) {
    /* multiple importance sampling, get background light pdf for ray
     * direction, and compute weight with respect to BSDF pdf */
    float pdf = background_light_pdf(kg, P_pick, ray->D);
    float mis_weight = power_heuristic(state->ray_pdf, pdf);

    return L * mis_weight;
  }
#  endif

  return L;
#else
  return make_float3(0.8f, 0.8f, 0.8f);
#endif
}

CCL_NAMESPACE_END
