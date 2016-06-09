//
//  SubsurfaceScattering.cpp
//  libraries/render-utils/src/
//
//  Created by Sam Gateau 6/3/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "SubsurfaceScattering.h"

#include <gpu/Context.h>
#include <gpu/StandardShaderLib.h>

#include "FramebufferCache.h"

#include "subsurfaceScattering_makeLUT_frag.h"

const int SubsurfaceScattering_FrameTransformSlot = 0;
const int SubsurfaceScattering_ParamsSlot = 1;
const int SubsurfaceScattering_DepthMapSlot = 0;
const int SubsurfaceScattering_NormalMapSlot = 1;

SubsurfaceScattering::SubsurfaceScattering() {
    Parameters parameters;
    _parametersBuffer = gpu::BufferView(std::make_shared<gpu::Buffer>(sizeof(Parameters), (const gpu::Byte*) &parameters));
}

void SubsurfaceScattering::configure(const Config& config) {
    
    if (config.depthThreshold != getCurvatureDepthThreshold()) {
        _parametersBuffer.edit<Parameters>().curvatureInfo.x = config.depthThreshold;
    }
}



gpu::PipelinePointer SubsurfaceScattering::getScatteringPipeline() {
    if (!_scatteringPipeline) {
        auto vs = gpu::StandardShaderLib::getDrawUnitQuadTexcoordVS();
        auto ps = gpu::StandardShaderLib::getDrawTextureOpaquePS();
        gpu::ShaderPointer program = gpu::Shader::createProgram(vs, ps);

        gpu::Shader::BindingSet slotBindings;
       // slotBindings.insert(gpu::Shader::Binding(std::string("blurParamsBuffer"), BlurTask_ParamsSlot));
       // slotBindings.insert(gpu::Shader::Binding(std::string("sourceMap"), BlurTask_SourceSlot));
        gpu::Shader::makeProgram(*program, slotBindings);

        gpu::StatePointer state = gpu::StatePointer(new gpu::State());

        // Stencil test the curvature pass for objects pixels only, not the background
      //  state->setStencilTest(true, 0xFF, gpu::State::StencilTest(0, 0xFF, gpu::NOT_EQUAL, gpu::State::STENCIL_OP_KEEP, gpu::State::STENCIL_OP_KEEP, gpu::State::STENCIL_OP_KEEP));

        _scatteringPipeline = gpu::Pipeline::create(program, state);
    }

    return _scatteringPipeline;
}


void SubsurfaceScattering::run(const render::SceneContextPointer& sceneContext, const render::RenderContextPointer& renderContext, const DeferredFrameTransformPointer& frameTransform, gpu::FramebufferPointer& curvatureFramebuffer) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    RenderArgs* args = renderContext->args;

    if (!_scatteringTable) {
        _scatteringTable = SubsurfaceScattering::generatePreIntegratedScattering(args);
    }

    auto pipeline = getScatteringPipeline();



    gpu::doInBatch(args->_context, [=](gpu::Batch& batch) {
        batch.enableStereo(false);

        batch.setViewportTransform(args->_viewport >> 1);
    /*    batch.setProjectionTransform(glm::mat4());
        batch.setViewTransform(Transform());

        Transform model;
        model.setTranslation(glm::vec3(sMin, tMin, 0.0f));
        model.setScale(glm::vec3(sWidth, tHeight, 1.0f));
        batch.setModelTransform(model);*/

        batch.setPipeline(pipeline);
        batch.setResourceTexture(0, _scatteringTable);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });

}





// Reference: http://www.altdevblogaday.com/2011/12/31/skin-shading-in-unity3d/

#include <cstdio>
#include <cmath>
#include <algorithm>

#define _PI 3.14159265358979523846

using namespace std;


double gaussian(float v, float r) {
    double g = (1.0 / sqrt(2.0 * _PI * v)) * exp(-(r*r) / (2.0 * v));
    return g;
}

vec3 scatter(double r) {
    // Values from GPU Gems 3 "Advanced Skin Rendering".
    // Originally taken from real life samples.
    static const double profile[][4] = {
        { 0.0064, 0.233, 0.455, 0.649 },
        { 0.0484, 0.100, 0.336, 0.344 },
        { 0.1870, 0.118, 0.198, 0.000 },
        { 0.5670, 0.113, 0.007, 0.007 },
        { 1.9900, 0.358, 0.004, 0.000 },
        { 7.4100, 0.078, 0.000, 0.000 }
    };
    static const int profileNum = 6;
    vec3 ret(0.0);
    for (int i = 0; i < profileNum; i++) {
        double g = gaussian(profile[i][0] * 1.414f, r);
        ret.x += g * profile[i][1];
        ret.y += g * profile[i][2];
        ret.z += g * profile[i][3];
    }

    return ret;
}

vec3 integrate(double cosTheta, double skinRadius) {
    // Angle from lighting direction.
    double theta = acos(cosTheta);
    vec3 totalWeights(0.0);
    vec3 totalLight(0.0);
    vec3 skinColour(1.0);

    double a = -(_PI);

    double inc = 0.001;
    if (cosTheta > 0)
        inc = 0.01;

    while (a <= (_PI)) {
        double sampleAngle = theta + a;
        double diffuse = cos(sampleAngle);
        if (diffuse < 0.0) diffuse = 0.0;
        if (diffuse > 1.0) diffuse = 1.0;

        // Distance.
        double sampleDist = abs(2.0 * skinRadius * sin(a * 0.5));

        // Profile Weight.
        vec3 weights = scatter(sampleDist);

        totalWeights += weights;
        totalLight.x += diffuse * weights.x * (skinColour.x * skinColour.x);
        totalLight.y += diffuse * weights.y * (skinColour.y * skinColour.y);
        totalLight.z += diffuse * weights.z * (skinColour.z * skinColour.z);
        a += inc;
    }

    vec3 result;
    result.x = totalLight.x / totalWeights.x;
    result.y = totalLight.y / totalWeights.y;
    result.z = totalLight.z / totalWeights.z;

    return result;
}

void diffuseScatter(gpu::TexturePointer& lut) {
    int width = lut->getWidth();
    int height = lut->getHeight();
    
    const int COMPONENT_COUNT = 4;
    std::vector<unsigned char> bytes(COMPONENT_COUNT * height * width);
    
    int index = 0;
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            // Lookup by: x: NDotL y: 1 / r
            float y = 2.0 * 1.0 / ((j + 1.0) / (double)height);
            float x = ((i / (double)width) * 2.0) - 1.0;
            vec3 val = integrate(x, y);

            // Convert to linear
            val.x = sqrt(val.x);
            val.y = sqrt(val.y);
            val.z = sqrt(val.z);

            // Convert to 24-bit image.
            unsigned char valI[3];
            if (val.x > 1.0) val.x = 1.0;
            if (val.y > 1.0) val.y = 1.0;
            if (val.z > 1.0) val.z = 1.0;
            valI[0] = (unsigned char)(val.x * 256.0);
            valI[1] = (unsigned char)(val.y * 256.0);
            valI[2] = (unsigned char)(val.z * 256.0);
            
            bytes[COMPONENT_COUNT * index] = valI[0];
            bytes[COMPONENT_COUNT * index + 1] = valI[1];
            bytes[COMPONENT_COUNT * index + 2] = valI[2];
            bytes[COMPONENT_COUNT * index + 3] = 255.0;
            
            index++;
        }
    }

    lut->assignStoredMip(0, gpu::Element::COLOR_RGBA_32, bytes.size(), bytes.data());
}


void diffuseScatterGPU(gpu::TexturePointer& profileMap, gpu::TexturePointer& lut, RenderArgs* args) {
    int width = lut->getWidth();
    int height = lut->getHeight();
    
    gpu::PipelinePointer makePipeline;
    {
        auto vs = gpu::StandardShaderLib::getDrawUnitQuadTexcoordVS();
        auto ps = gpu::Shader::createPixel(std::string(subsurfaceScattering_makeLUT_frag));
        gpu::ShaderPointer program = gpu::Shader::createProgram(vs, ps);
        
        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding(std::string("profileMap"), 0));
        // slotBindings.insert(gpu::Shader::Binding(std::string("sourceMap"), BlurTask_SourceSlot));
        gpu::Shader::makeProgram(*program, slotBindings);
        
        gpu::StatePointer state = gpu::StatePointer(new gpu::State());
        
        // Stencil test the curvature pass for objects pixels only, not the background
        //  state->setStencilTest(true, 0xFF, gpu::State::StencilTest(0, 0xFF, gpu::NOT_EQUAL, gpu::State::STENCIL_OP_KEEP, gpu::State::STENCIL_OP_KEEP, gpu::State::STENCIL_OP_KEEP));
        
        makePipeline = gpu::Pipeline::create(program, state);
    }

    auto makeFramebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create());
    makeFramebuffer->setRenderBuffer(0, lut);
    
    gpu::doInBatch(args->_context, [=](gpu::Batch& batch) {
        batch.enableStereo(false);
        
        batch.setViewportTransform(glm::ivec4(0, 0, width, height));
     
        batch.setFramebuffer(makeFramebuffer);
        batch.setPipeline(makePipeline);
        batch.setResourceTexture(0, profileMap);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
        batch.setResourceTexture(0, nullptr);
        batch.setPipeline(nullptr);
        batch.setFramebuffer(nullptr);
        
    });
}

void diffuseProfile(gpu::TexturePointer& profile) {
    int width = profile->getWidth();
    int height = profile->getHeight();
    
    const int COMPONENT_COUNT = 4;
    std::vector<unsigned char> bytes(COMPONENT_COUNT * height * width);

    int index = 0;
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            float y = (double)(i + 1.0) / (double)width;
            vec3 val = scatter(y * 2.0f);

            // Convert to 24-bit image.
            unsigned char valI[3];
            if (val.x > 1.0) val.x = 1.0;
            if (val.y > 1.0) val.y = 1.0;
            if (val.z > 1.0) val.z = 1.0;
            valI[0] = (unsigned char)(val.x * 255.0);
            valI[1] = (unsigned char)(val.y * 255.0);
            valI[2] = (unsigned char)(val.z * 255.0);

            bytes[COMPONENT_COUNT * index] = valI[0];
            bytes[COMPONENT_COUNT * index + 1] = valI[1];
            bytes[COMPONENT_COUNT * index + 2] = valI[2];
            bytes[COMPONENT_COUNT * index + 3] = 255.0;

            index++;
        }
    }

    
    profile->assignStoredMip(0, gpu::Element::COLOR_RGBA_32, bytes.size(), bytes.data());
}



gpu::TexturePointer SubsurfaceScattering::generatePreIntegratedScattering(RenderArgs* args) {
    
    auto profileMap = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, 128, 1, gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR)));
    diffuseProfile(profileMap);

    const int WIDTH = 128;
    const int HEIGHT = 128;
    auto scatteringLUT = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, WIDTH, HEIGHT));
 //   diffuseScatter(scatteringLUT);
    diffuseScatterGPU(profileMap, scatteringLUT, args);
    return scatteringLUT;
}

