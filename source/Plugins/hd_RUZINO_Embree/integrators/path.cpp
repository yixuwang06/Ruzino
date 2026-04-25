#include "path.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "../surfaceInteraction.h"
RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

VtValue PathIntegrator::Li(const GfRay& ray, std::default_random_engine& random)
{
    std::uniform_real_distribution<float> uniform_dist(
        0.0f, 1.0f - std::numeric_limits<float>::epsilon());
    std::function<float()> uniform_float = std::bind(uniform_dist, random);

    auto color = EstimateOutGoingRadiance(ray, uniform_float, 0);

    return VtValue(GfVec3f(color[0], color[1], color[2]));
}

GfVec3f PathIntegrator::EstimateOutGoingRadiance(
    const GfRay& ray,
    const std::function<float()>& uniform_float,
    int recursion_depth)
{
    if (recursion_depth >= 50) {
        return {};
    }

    SurfaceInteraction si;
    if (!Intersect(ray, si)) {
        if (recursion_depth == 0) {
            GfVec3f intersectPos;
            return IntersectLights(ray, intersectPos);
        }
        return GfVec3f{ 0, 0, 0 };
    }

    // This can be customized : Do we want to see the lights? (Other than dome
    // lights?)
    if (recursion_depth == 0) {
    }

    // Flip the normal if opposite
    if (GfDot(si.shadingNormal, ray.GetDirection()) > 0) {
        si.flipNormal();
        si.PrepareTransforms();
    }

    GfVec3f color{ 0 };
    GfVec3f directLight = EstimateDirectLight(si, uniform_float);

    GfVec3f globalLight = GfVec3f{ 0.f };
    GfVec3f wi;
    float pdf = 0.0f;
    GfVec3f brdf = si.Sample(wi, pdf, uniform_float);
    float cosTheta = std::max(0.0f, GfDot(si.shadingNormal, wi));

    if (pdf > 1E-6f && cosTheta > 0.0f) {
        float continueProb = recursion_depth < 3 ? 1.0f : 0.8f;
        if (uniform_float() < continueProb) {
            GfVec3f offsetNormal =
                GfDot(wi, si.geometricNormal) >= 0.0f ? si.geometricNormal
                                                      : -si.geometricNormal;
            GfRay bounceRay;
            bounceRay.SetPointAndDirection(
                si.position + 0.0001f * offsetNormal, wi);

            GfVec3f bouncedRadiance = EstimateOutGoingRadiance(
                bounceRay, uniform_float, recursion_depth + 1);
            globalLight = GfCompMult(brdf, bouncedRadiance) *
                          (cosTheta / (pdf * continueProb));
        }
    }

    color = directLight + globalLight;

    return color;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
