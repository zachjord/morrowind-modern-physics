#ifndef OPENMW_MWRENDER_BLOODRENDERER_H
#define OPENMW_MWRENDER_BLOODRENDERER_H

#include <cstddef>
#include <limits>

#include <osg/Vec3f>
#include <osg/ref_ptr>

namespace osg
{
    class Group;
    class Geometry;
    class Vec3Array;
    class Vec4Array;
    class DrawArrays;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWPhysics
{
    class BloodSim;
}

namespace MWRender
{
    /// One mark of blood, on its own: a soft-edged disc at center, facing along normal (for marks on moving things,
    /// see Animation::addBloodMark). Shaders not yet made.
    osg::ref_ptr<osg::Geometry> makeBloodMark(const osg::Vec3f& center, const osg::Vec3f& normal, float radius);

    /// Draws a BloodSim: droplets as short streaks along their flight, marks as soft-edged, slightly ragged discs
    /// on their surface. Plain geometry made each frame (droplets) or when the marks change; no textures.
    class BloodRenderer
    {
    public:
        BloodRenderer(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem);
        ~BloodRenderer();

        BloodRenderer(const BloodRenderer&) = delete;
        BloodRenderer& operator=(const BloodRenderer&) = delete;

        void update(const MWPhysics::BloodSim& sim);

    private:
        void updateDrops(const MWPhysics::BloodSim& sim);
        void updateStains(const MWPhysics::BloodSim& sim);

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Group> mRoot;

        osg::ref_ptr<osg::Geometry> mDrops;
        osg::ref_ptr<osg::Vec3Array> mDropVertices;
        osg::ref_ptr<osg::Vec4Array> mDropColors;
        osg::ref_ptr<osg::DrawArrays> mDropPrimitives;

        osg::ref_ptr<osg::Geometry> mStains;
        osg::ref_ptr<osg::Vec3Array> mStainVertices;
        osg::ref_ptr<osg::Vec3Array> mStainNormals;
        osg::ref_ptr<osg::Vec4Array> mStainColors;
        osg::ref_ptr<osg::DrawArrays> mStainPrimitives;
        std::size_t mStainRevision = std::numeric_limits<std::size_t>::max();
    };
}

#endif
