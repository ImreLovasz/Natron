//  Natron
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
*Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
*contact: immarespond at gmail dot com
*
*/

#include "Image.h"

#include <QDebug>

#include "Engine/ImageParams.h"

using namespace Natron;



ImageKey::ImageKey()
: KeyHelper<U64>()
, _nodeHashKey(0)
, _time(0)
, _renderScale()
, _view(0)
, _pixelAspect(1)
{}


ImageKey::ImageKey(U64 nodeHashKey,
         SequenceTime time,
         RenderScale scale,
         int view,
         double pixelAspect)
: KeyHelper<U64>()
, _nodeHashKey(nodeHashKey)
, _time(time)
, _view(view)
, _pixelAspect(pixelAspect)
{ _renderScale = scale; }

void ImageKey::fillHash(Hash64* hash) const {
    hash->append(_nodeHashKey);
    hash->append(_renderScale.x);
    hash->append(_renderScale.y);
    hash->append(_time);
    hash->append(_view);
    hash->append(_pixelAspect);
}



bool ImageKey::operator==(const ImageKey& other) const {
    return _nodeHashKey == other._nodeHashKey &&
    _renderScale.x == other._renderScale.x &&
    _renderScale.y == other._renderScale.y &&
    _time == other._time &&
    _view == other._view &&
    _pixelAspect == other._pixelAspect;
    
}




RectI Natron::Bitmap::minimalNonMarkedBbox(const RectI& roi) const
{
    /*if we rendered everything we just append
     a NULL box to indicate we rendered it all.*/
//    if(!memchr(_map.get(),0,_rod.area())){
//        ret.push_back(RectI());
//        return ret;
//    }
    
    RectI bbox = roi;
    //find bottom
    for (int i = bbox.bottom(); i < bbox.top();++i) {
        char* buf = &_map[(i-_rod.bottom())*_rod.width()];
        if(!memchr(buf, 0, _rod.width())){
            bbox.set_bottom(bbox.bottom()+1);
        } else {
            break;
        }
    }

    //find top (will do zero iteration if the bbox is already empty)
    for (int i = bbox.top()-1; i >= bbox.bottom();--i) {
        char* buf = &_map[(i-_rod.bottom())*_rod.width()];
        if (!memchr(buf, 0, _rod.width())) {
            bbox.set_top(bbox.top()-1);
        } else {
            break;
        }
    }

    // avoid making bbox.width() iterations for nothing
    if (bbox.isNull()) {
        return bbox;
    }

    //find left
    for (int j = bbox.left(); j < bbox.right(); ++j) {
        bool shouldStop = false;
        for (int i = bbox.bottom(); i < bbox.top(); ++i) {
            if (!_map[(i-_rod.bottom())*_rod.width() + (j-_rod.left())]) {
                shouldStop = true;
                break;
            }
        }
        if (!shouldStop) {
            bbox.set_left(bbox.left()+1);
        } else {
            break;
        }
    }

    //find right
    for (int j = bbox.right()-1; j >= bbox.left(); --j) {
        bool shouldStop = false;
        for (int i = bbox.bottom(); i < bbox.top(); ++i) {
            if (!_map[(i-_rod.bottom())*_rod.width() + (j-_rod.left())]) {
                shouldStop = true;
                break;
            }
        }
        if (!shouldStop) {
            bbox.set_right(bbox.right()-1);
        } else {
            break;
        }
    }
    return bbox;
}

std::list<RectI> Natron::Bitmap::minimalNonMarkedRects(const RectI& roi) const
{
    std::list<RectI> ret;

    RectI bboxM = minimalNonMarkedBbox(roi);
//#define NATRON_BITMAP_DISABLE_OPTIMIZATION
#ifdef NATRON_BITMAP_DISABLE_OPTIMIZATION
    if (!bboxM.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxM);
    }
#else
    if (bboxM.isNull()) {
        return ret; // return an empty rectangle list
    }

    // optimization by Fred, Jan 31, 2014
    //
    // Now that we have the smallest enclosing bounding box,
    // let's try to find rectangles for the bottom, the top,
    // the left and the right part.
    // This happens quite often, for example when zooming out
    // (in this case the area to compute is formed of A, B, C and D,
    // and X is already rendered), or when panning (in this case the area
    // is just two rectangles, e.g. A and C, and the rectangles B, D and
    // X are already rendered).
    // The rectangles A, B, C and D from the following drawing are just
    // zeroes, and X contains zeroes and ones.
    //
    // BBBBBBBBBBBBBB
    // BBBBBBBBBBBBBB
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // AAAAAAAAAAAAAA

    // First, find if there's an "A" rectangle, and push it to the result
    //find bottom
    RectI bboxX = bboxM;
    RectI bboxA = bboxX;
    bboxA.set_top(bboxX.bottom());
    for (int i = bboxX.bottom(); i < bboxX.top();++i) {
        char* buf = &_map[(i-_rod.bottom())*_rod.width()];
        if (!memchr(buf, 1, _rod.width())) {
            bboxX.set_bottom(bboxX.bottom()+1);
            bboxA.set_top(bboxX.bottom());
        } else {
            break;
        }
    }
    if (!bboxA.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxA);
    }

    // Now, find the "B" rectangle
    //find top
    RectI bboxB = bboxX;
    bboxB.set_bottom(bboxX.top());
    for (int i = bboxX.top()-1; i >= bboxX.bottom();--i) {
        char* buf = &_map[(i-_rod.bottom())*_rod.width()];
        if (!memchr(buf, 1, _rod.width())) {
            bboxX.set_top(bboxX.top()-1);
            bboxB.set_bottom(bboxX.top());
        } else {
            break;
        }
    }
    if (!bboxB.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxB);
    }

    //find left
    RectI bboxC = bboxX;
    bboxC.set_right(bboxX.left());
    for (int j = bboxX.left(); j < bboxX.right(); ++j) {
        bool shouldStop = false;
        for (int i = bboxX.bottom(); i < bboxX.top(); ++i) {
            if (_map[(i-_rod.bottom())*_rod.width()+(j-_rod.left())]) {
                shouldStop = true;
                break;
            }
        }
        if (!shouldStop) {
            bboxX.set_left(bboxX.left()+1);
            bboxC.set_right(bboxX.left());
        } else {
            break;
        }
    }
    if (!bboxC.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxC);
    }

    //find right
    RectI bboxD = bboxX;
    bboxD.set_left(bboxX.right());
    for (int j = bboxX.right()-1; j >= bboxX.left(); --j) {
        bool shouldStop = false;
        for (int i = bboxX.bottom(); i < bboxX.top(); ++i) {
            if (_map[(i-_rod.bottom())*_rod.width()+(j-_rod.left())]) {
                shouldStop = true;
                break;
            }
        }
        if (!shouldStop) {
            bboxX.set_right(bboxX.right()-1);
            bboxD.set_left(bboxX.right());
        } else {
            break;
        }
    }
    if (!bboxD.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxD);
    }

    assert(bboxA.bottom() == bboxM.bottom());
    assert(bboxA.left() == bboxM.left());
    assert(bboxA.right() == bboxM.right());
    assert(bboxA.top() == bboxX.bottom());

    assert(bboxB.top() == bboxM.top());
    assert(bboxB.left() == bboxM.left());
    assert(bboxB.right() == bboxM.right());
    assert(bboxB.bottom() == bboxX.top());

    assert(bboxC.top() == bboxX.top());
    assert(bboxC.left() == bboxM.left());
    assert(bboxC.right() == bboxX.left());
    assert(bboxC.bottom() == bboxX.bottom());

    assert(bboxD.top() == bboxX.top());
    assert(bboxD.left() == bboxX.right());
    assert(bboxD.right() == bboxM.right());
    assert(bboxD.bottom() == bboxX.bottom());

    // get the bounding box of what's left (the X rectangle in the drawing above)
    bboxX = minimalNonMarkedBbox(bboxX);
    if (!bboxX.isNull()) { // empty boxes should not be pushed
        ret.push_back(bboxX);
    }

#endif // NATRON_BITMAP_DISABLE_OPTIMIZATION
    qDebug() << "render " << ret.size() << " rectangles";
    for (std::list<RectI>::const_iterator it = ret.begin(); it != ret.end(); ++it) {
        qDebug() << "rect: " << "x1= "<<  it->x1 << " , x2= "<< it->x2 << " , y1= " << it->y1 << " , y2= " << it->y2;
    }
    return ret;
}

void Natron::Bitmap::markForRendered(const RectI& roi){
    for (int i = roi.bottom(); i < roi.top();++i) {
        char* buf = &_map[(i-_rod.bottom())*_rod.width() + (roi.left() - _rod.left())];
        memset(buf, 1, roi.width());
    }
}

const char* Natron::Bitmap::getBitmapAt(int x,int y) const
{
    if (x >= _rod.left() && x < _rod.right() && y >= _rod.bottom() && y < _rod.top()) {
        return _map.get() + (y - _rod.bottom()) * _rod.width() + (x - _rod.left());
    } else {
        return NULL;
    }
}

char* Natron::Bitmap::getBitmapAt(int x,int y)
{
    if (x >= _rod.left() && x < _rod.right() && y >= _rod.bottom() && y < _rod.top()) {
        return _map.get() + (y - _rod.bottom()) * _rod.width() + (x - _rod.left());
    } else {
        return NULL;
    }
}

Image::Image(const ImageKey& key,const NonKeyParams& params,bool restore,const std::string& path):
CacheEntryHelper<float,ImageKey>(key,params,restore,path)
, _components(dynamic_cast<const ImageParams&>(params).getComponents())
,_bitmap(dynamic_cast<const ImageParams&>(params).getRoD())
{
}

/*This constructor can be used to allocate a local Image. The deallocation should
 then be handled by the user. Note that no view number is passed in parameter
 as it is not needed.*/
Image::Image(ImageComponents components,const RectI& regionOfDefinition,RenderScale scale,SequenceTime time)
: CacheEntryHelper<float,ImageKey>(makeKey(0,time,scale,0),
                                   ImageParams(0, regionOfDefinition, false ,components,-1,0,std::map<int,std::vector<RangeD> >()),
                                   false,"")
, _components(components)
, _bitmap(regionOfDefinition)
{
    // NOTE: before removing the following assert, please explain why an empty image may happen
    assert(!regionOfDefinition.isNull());
}

ImageKey Image::makeKey(U64 nodeHashKey,
                        SequenceTime time,
                        RenderScale scale,
                        int view){
    return ImageKey(nodeHashKey,time,scale,view);
}

boost::shared_ptr<ImageParams> Image::makeParams(int cost,const RectI& rod,bool isRoDProjectFormat,ImageComponents components,
                                                 int inputNbIdentity,int inputTimeIdentity,
                                                 const std::map<int, std::vector<RangeD> >& framesNeeded) {
    return boost::shared_ptr<ImageParams>(new ImageParams(cost,rod,isRoDProjectFormat,components,inputNbIdentity,inputTimeIdentity,framesNeeded));
}


void Natron::Image::copy(const Natron::Image& other)
{
    // NOTE: before removing the following asserts, please explain why an empty image may happen
    assert(!getRoD().isNull());
    assert(!other.getRoD().isNull());
    RectI intersection;
    getRoD().intersect(other.getRoD(), &intersection);
    
    if (intersection.isNull()) {
        return;
    }
    
    const float* src = other.pixelAt(0, 0);
    float* dst = pixelAt(0, 0);
    memcpy(dst, src, intersection.area());
}

void Natron::Image::fill(const RectI& rect,float r,float g,float b,float a) {
    ImageComponents comps = getComponents();
    for (int i = rect.bottom(); i < rect.top();++i) {
        float* dst = pixelAt(rect.left(),i);
        for (int j = 0; j < rect.width();++j) {
            switch (comps) {
                case ImageComponentAlpha:
                    dst[j * 4] = a;
                    break;
                case ImageComponentRGB:
                    dst[j * 4] = r;
                    dst[j * 4 + 1] = g;
                    dst[j * 4 + 2] = b;
                    break;
                case ImageComponentRGBA:
                    dst[j * 4] = r;
                    dst[j * 4 + 1] = g;
                    dst[j * 4 + 2] = b;
                    dst[j * 4 + 3] = a;
                    break;
                case ImageComponentNone:
                default:
                    break;
            }
        }
    }
}

float* Image::pixelAt(int x,int y){
    const RectI& rod = _bitmap.getRoD();
    int compsCount = getElementsCountForComponents(getComponents());
    return this->_data.writable() + (y-rod.bottom()) * compsCount * rod.width() + (x-rod.left()) * compsCount;
}

const float* Image::pixelAt(int x,int y) const {
    const RectI& rod = _bitmap.getRoD();
    int compsCount = getElementsCountForComponents(getComponents());
    if (x >= rod.left() && x < rod.right() && y >= rod.bottom() && y < rod.top()) {
        return this->_data.readable() + (y-rod.bottom()) * compsCount * rod.width() + (x-rod.left()) * compsCount;
    } else {
        return NULL;
    }
}

void Image::scaled(Natron::Image* output,double sx,double sy) const
{
    
    assert((output->getRoD().width() == ((double)getRoD().width() * sx)) &&
           (output->getRoD().height() == ((double)getRoD().height() * sy)));
    
    
    const RectI& dstRoD = output->getRoD();
    const RectI& srcRoD = getRoD();
    
    double yScaleFactor = (double)dstRoD.height() / (double)srcRoD.height();
    double xScaleFactor = (double)dstRoD.width() / (double)srcRoD.width();
    
    int elementsCount = getElementsCountForComponents(_components);
    
    for (int y = dstRoD.y1; y < dstRoD.y2; ++y) {
        
        
        double ysrc = ((double)y / yScaleFactor) + srcRoD.y1;
        
        int fy = std::floor(ysrc);
        int cy = fy + 1;
        double dy = std::max(0., std::min(ysrc - fy, 1.));
        
        const float* srcPixelsFloor = pixelAt(srcRoD.x1, fy);
        const float* srcPixelsCeil = pixelAt(srcRoD.x1, cy);
        float* dstPixels = output->pixelAt(dstRoD.x1,y);

        const char* srcBitmapFloor = getBitmapAt(srcRoD.x1, fy);
        const char* srcBitmapCeil = getBitmapAt(srcRoD.x1, cy);
        char* dstBitmap = output->getBitmapAt(dstRoD.x1, y);
        
        for (int x = dstRoD.x1; x < dstRoD.x2; ++x) {
            double xsrc = ((double)x / xScaleFactor) + srcRoD.x1;
            
            int fx = std::floor(xsrc);
            int cx = fx + 1;
            double dx = std::max(0., std::min(xsrc - fx, 1.));
            

            for (int i = 0; i < elementsCount ;++i) {
                
                const double Icc = (!srcPixelsFloor || fx < srcRoD.x1) ? 0. : srcPixelsFloor[fx * elementsCount + i];
                const double Inc = (!srcPixelsFloor || cx >= srcRoD.x2) ? 0. : srcPixelsFloor[cx * elementsCount + i];
                const double Icn = (!srcPixelsCeil || fx < srcRoD.x1) ? 0. : srcPixelsCeil[fx * elementsCount + i];
                const double Inn = (!srcPixelsCeil || cx >= srcRoD.x2) ? 0. : srcPixelsCeil[cx * elementsCount + i];
                *dstPixels++ = Icc + dx*(Inc-Icc + dy*(Icc+Inn-Icn-Inc)) + dy*(Icn-Icc);
            }
            
            
            const double Icc = (!srcBitmapFloor || fx < srcRoD.x1) ? 0. : srcPixelsFloor[fx];
            const double Inc = (!srcBitmapFloor || cx >= srcRoD.x2) ? 0. : srcPixelsFloor[cx];
            const double Icn = (!srcBitmapCeil || fx < srcRoD.x1) ? 0. : srcPixelsCeil[fx];
            const double Inn = (!srcBitmapCeil || cx >= srcRoD.x2) ? 0. : srcPixelsCeil[cx];
            
            ///average the 4 neighbooring pixels
            *dstBitmap++ = (Icc + Inc + Icn + Inn) / 4.;

            
        }
    }
}