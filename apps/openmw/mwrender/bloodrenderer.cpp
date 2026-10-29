#include "bloodrenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <osg/BlendFunc>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Math>
#include <osg/PolygonOffset>
#include <osg/StateSet>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>

#include "../mwphysics/bloodsim.hpp"

#include "renderbin.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        // Fresh blood is dark; lumps darker and browner.
        const osg::Vec4f sDropColor(0.32f, 0.015f, 0.015f, 0.95f);
        const osg::Vec4f sChunkColor(0.36f, 0.08f, 0.06f, 1.f);
        const osg::Vec4f sStainColor(0.26f, 0.01f, 0.01f, 0.9f);
        const osg::Vec4f sChunkStainColor(0.2f, 0.03f, 0.02f, 0.95f);

        constexpr int sStainSegments = 12;

        // The same rough edge each time for the same mark (it is rebuilt whenever any mark changes).
        float edgeNoise(const osg::Vec3f& position, int segment)
        {
            std::uint32_t h = static_cast<std::uint32_t>(std::lround(position.x() * 7.f)) * 73856093u
                ^ static_cast<std::uint32_t>(std::lround(position.y() * 7.f)) * 19349663u
                ^ static_cast<std::uint32_t>(std::lround(position.z() * 7.f)) * 83492791u
                ^ static_cast<std::uint32_t>(segment) * 2654435761u;
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            return static_cast<float>(h & 0xffff) / 65535.f;
        }

        void tangents(const osg::Vec3f& normal, osg::Vec3f& u, osg::Vec3f& v)
        {
            const osg::Vec3f helper = std::abs(normal.z()) < 0.9f ? osg::Vec3f(0, 0, 1) : osg::Vec3f(1, 0, 0);
            u = helper ^ normal;
            u.normalize();
            v = normal ^ u;
        }

        osg::ref_ptr<osg::StateSet> makeStateSet(bool lit)
        {
            osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
            stateSet->setAttributeAndModes(
                new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));
            // Drawn after what they lie on, over it, without hiding what is behind them.
            stateSet->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0.0, 1.0, false));
            stateSet->setRenderBinDetails(RenderBin_DepthSorted, "DepthSortedBin");
            if (lit)
                stateSet->setAttributeAndModes(new osg::PolygonOffset(
                    SceneUtil::AutoDepth::isReversed() ? 1.f : -1.f, SceneUtil::AutoDepth::isReversed() ? 1.f : -1.f));
            stateSet->setAttribute(new SceneUtil::Material({ .mSpecular = { 0.3f, 0.3f, 0.3f, 1.f }, // wet
                .mShininess = 40.f,
                .mVertexColorMode = SceneUtil::VertexColorModes::AmbientAndDiffuse }));
            return stateSet;
        }

        osg::ref_ptr<osg::Geometry> makeGeometry()
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            geometry->setDataVariance(osg::Object::DYNAMIC);
            geometry->setCullingActive(false); // its bounds change all the time
            return geometry;
        }
    }

    BloodRenderer::BloodRenderer(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem)
        : mParent(std::move(parent))
        , mRoot(new osg::Group)
    {
        mRoot->setNodeMask(Mask_Effect);
        mRoot->setDataVariance(osg::Object::DYNAMIC);

        mDrops = makeGeometry();
        mDropVertices = new osg::Vec3Array;
        mDropColors = new osg::Vec4Array;
        mDropPrimitives = new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0);
        mDrops->setVertexArray(mDropVertices);
        mDrops->setColorArray(mDropColors, osg::Array::BIND_PER_VERTEX);
        mDrops->addPrimitiveSet(mDropPrimitives);
        mDrops->setStateSet(makeStateSet(false));
        mRoot->addChild(mDrops);

        mStains = makeGeometry();
        mStainVertices = new osg::Vec3Array;
        mStainNormals = new osg::Vec3Array;
        mStainColors = new osg::Vec4Array;
        mStainPrimitives = new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 0);
        mStains->setVertexArray(mStainVertices);
        mStains->setNormalArray(mStainNormals, osg::Array::BIND_PER_VERTEX);
        mStains->setColorArray(mStainColors, osg::Array::BIND_PER_VERTEX);
        mStains->addPrimitiveSet(mStainPrimitives);
        mStains->setStateSet(makeStateSet(true));
        mRoot->addChild(mStains);

        // Streaks lit flat (the "debug" shader: vertex colors as they are), marks lit as the world is.
        Resource::SceneManager* sceneManager = resourceSystem->getSceneManager();
        sceneManager->recreateShaders(mDrops, "debug");
        sceneManager->recreateShaders(mStains);

        mParent->addChild(mRoot);
    }

    BloodRenderer::~BloodRenderer()
    {
        mParent->removeChild(mRoot);
    }

    void BloodRenderer::update(const MWPhysics::BloodSim& sim)
    {
        updateDrops(sim);
        updateStains(sim);
    }

    void BloodRenderer::updateDrops(const MWPhysics::BloodSim& sim)
    {
        mDropVertices->clear();
        mDropColors->clear();
        for (const MWPhysics::BloodDrop& drop : sim.getDrops())
        {
            // A streak behind it, as fast things look; at least a little one.
            osg::Vec3f tail = drop.mVelocity * -0.012f;
            const float length = tail.length();
            const float minLength = drop.mChunk ? 1.6f : 0.7f;
            if (length < minLength)
                tail = length > 1e-4f ? tail * (minLength / length) : osg::Vec3f(0, 0, minLength);
            mDropVertices->push_back(drop.mPosition);
            mDropVertices->push_back(drop.mPosition + tail);
            const osg::Vec4f& color = drop.mChunk ? sChunkColor : sDropColor;
            mDropColors->push_back(color);
            mDropColors->push_back(osg::Vec4f(color.r(), color.g(), color.b(), color.a() * 0.3f));
        }
        mDropPrimitives->setCount(static_cast<GLsizei>(mDropVertices->size()));
        mDropVertices->dirty();
        mDropColors->dirty();
        mDrops->dirtyBound();
    }

    void BloodRenderer::updateStains(const MWPhysics::BloodSim& sim)
    {
        if (sim.getStainRevision() == mStainRevision)
            return;
        mStainRevision = sim.getStainRevision();

        mStainVertices->clear();
        mStainNormals->clear();
        mStainColors->clear();
        for (const MWPhysics::BloodStain& stain : sim.getStains())
        {
            osg::Vec3f u;
            osg::Vec3f v;
            tangents(stain.mNormal, u, v);
            // Just off the surface (and drawn in front of it, see PolygonOffset).
            const osg::Vec3f center = stain.mPosition + stain.mNormal * 0.15f;
            const osg::Vec4f& color = stain.mChunk ? sChunkStainColor : sStainColor;
            const osg::Vec4f edge(color.r(), color.g(), color.b(), 0.f);

            // A fan: solid in the middle, fading out over the ragged rim.
            osg::Vec3f inner[sStainSegments];
            osg::Vec3f outer[sStainSegments];
            for (int i = 0; i < sStainSegments; ++i)
            {
                const float angle = 2.f * osg::PIf * static_cast<float>(i) / sStainSegments;
                const osg::Vec3f direction = u * std::cos(angle) + v * std::sin(angle);
                const float rim = stain.mRadius * (0.75f + 0.45f * edgeNoise(stain.mPosition, i));
                inner[i] = center + direction * (rim * 0.7f);
                outer[i] = center + direction * rim;
            }
            for (int i = 0; i < sStainSegments; ++i)
            {
                const int j = (i + 1) % sStainSegments;
                const osg::Vec3f triangles[]
                    = { center, inner[i], inner[j], inner[i], outer[i], outer[j], inner[i], outer[j], inner[j] };
                const bool solid[] = { true, true, true, true, false, false, true, false, true };
                for (int k = 0; k < 9; ++k)
                {
                    mStainVertices->push_back(triangles[k]);
                    mStainNormals->push_back(stain.mNormal);
                    mStainColors->push_back(solid[k] ? color : edge);
                }
            }
        }
        mStainPrimitives->setCount(static_cast<GLsizei>(mStainVertices->size()));
        mStainVertices->dirty();
        mStainNormals->dirty();
        mStainColors->dirty();
        mStains->dirtyBound();
    }
}
