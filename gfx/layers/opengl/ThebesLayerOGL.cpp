/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Corporation code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Bas Schouten <bschouten@mozilla.org>
 *   Vladimir Vukicevic <vladimir@pobox.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "mozilla/layers/PLayers.h"
#include "mozilla/layers/ShadowLayers.h"

#include "ThebesLayerBuffer.h"
#include "ThebesLayerOGL.h"
#include "gfxUtils.h"
#include "gfxTeeSurface.h"

namespace mozilla {
namespace layers {

using gl::GLContext;
using gl::TextureImage;

static const int ALLOW_REPEAT = ThebesLayerBuffer::ALLOW_REPEAT;

// BindAndDrawQuadWithTextureRect can work with either GL_REPEAT (preferred)
// or GL_CLAMP_TO_EDGE textures. If ALLOW_REPEAT is set in aFlags, we
// select based on whether REPEAT is valid for non-power-of-two textures --
// if we have NPOT support we use it, otherwise we stick with CLAMP_TO_EDGE and
// decompose.
// If ALLOW_REPEAT is not set, we always use GL_CLAMP_TO_EDGE.
static already_AddRefed<TextureImage>
CreateClampOrRepeatTextureImage(GLContext *aGl,
                                const nsIntSize& aSize,
                                TextureImage::ContentType aContentType,
                                PRUint32 aFlags)
{
  GLenum wrapMode = LOCAL_GL_CLAMP_TO_EDGE;
  if ((aFlags & ALLOW_REPEAT) &&
      (aGl->IsExtensionSupported(GLContext::ARB_texture_non_power_of_two) ||
       aGl->IsExtensionSupported(GLContext::OES_texture_npot)))
  {
    wrapMode = LOCAL_GL_REPEAT;
  }

  return aGl->CreateTextureImage(aSize, aContentType, wrapMode);
}

// |aTexCoordRect| is the rectangle from the texture that we want to
// draw using the given program.  The program already has a necessary
// offset and scale, so the geometry that needs to be drawn is a unit
// square from 0,0 to 1,1.
//
// |aTexSize| is the actual size of the texture, as it can be larger
// than the rectangle given by |aTexCoordRect|.
static void
BindAndDrawQuadWithTextureRect(GLContext* aGl,
                               LayerProgram *aProg,
                               const nsIntRect& aTexCoordRect,
                               const nsIntSize& aTexSize,
                               GLenum aWrapMode)
{
  GLuint vertAttribIndex =
    aProg->AttribLocation(LayerProgram::VertexAttrib);
  GLuint texCoordAttribIndex =
    aProg->AttribLocation(LayerProgram::TexCoordAttrib);
  NS_ASSERTION(texCoordAttribIndex != GLuint(-1), "no texture coords?");

  // clear any bound VBO so that glVertexAttribPointer() goes back to
  // "pointer mode"
  aGl->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);

  // Given what we know about these textures and coordinates, we can
  // compute fmod(t, 1.0f) to get the same texture coordinate out.  If
  // the texCoordRect dimension is < 0 or > width/height, then we have
  // wraparound that we need to deal with by drawing multiple quads,
  // because we can't rely on full non-power-of-two texture support
  // (which is required for the REPEAT wrap mode).

  GLContext::RectTriangles rects;

  if (aWrapMode == LOCAL_GL_REPEAT) {
    rects.addRect(/* dest rectangle */
                  0.0f, 0.0f, 1.0f, 1.0f,
                  /* tex coords */
                  aTexCoordRect.x / GLfloat(aTexSize.width),
                  aTexCoordRect.y / GLfloat(aTexSize.height),
                  aTexCoordRect.XMost() / GLfloat(aTexSize.width),
                  aTexCoordRect.YMost() / GLfloat(aTexSize.height));
  } else {
    GLContext::DecomposeIntoNoRepeatTriangles(aTexCoordRect, aTexSize, rects);
  }

  // vertex position buffer is 2 floats, not normalized, 0 stride.
  aGl->fVertexAttribPointer(vertAttribIndex, 2,
                            LOCAL_GL_FLOAT, LOCAL_GL_FALSE, 0,
                            rects.vertexPointer());

  // texture coord buffer is 2 floats, not normalized, 0 stride.
  aGl->fVertexAttribPointer(texCoordAttribIndex, 2,
                            LOCAL_GL_FLOAT, LOCAL_GL_FALSE, 0,
                            rects.texCoordPointer());

  {
    aGl->fEnableVertexAttribArray(texCoordAttribIndex);
    {
      aGl->fEnableVertexAttribArray(vertAttribIndex);

      aGl->fDrawArrays(LOCAL_GL_TRIANGLES, 0, rects.elements());

      aGl->fDisableVertexAttribArray(vertAttribIndex);
    }
    aGl->fDisableVertexAttribArray(texCoordAttribIndex);
  }
}

static void
SetAntialiasingFlags(Layer* aLayer, gfxContext* aTarget)
{
  nsRefPtr<gfxASurface> surface = aTarget->CurrentSurface();
  if (surface->GetContentType() != gfxASurface::CONTENT_COLOR_ALPHA) {
    // Destination doesn't have alpha channel; no need to set any special flags
    return;
  }

  surface->SetSubpixelAntialiasingEnabled(
      !(aLayer->GetContentFlags() & Layer::CONTENT_COMPONENT_ALPHA));
}

class ThebesLayerBufferOGL
{
  NS_INLINE_DECL_REFCOUNTING(ThebesLayerBufferOGL)
public:
  typedef TextureImage::ContentType ContentType;
  typedef ThebesLayerBuffer::PaintState PaintState;

  ThebesLayerBufferOGL(ThebesLayer* aLayer, LayerOGL* aOGLLayer)
    : mLayer(aLayer)
    , mOGLLayer(aOGLLayer)
  {}
  virtual ~ThebesLayerBufferOGL() {}

  enum { PAINT_WILL_RESAMPLE = ThebesLayerBuffer::PAINT_WILL_RESAMPLE };
  virtual PaintState BeginPaint(ContentType aContentType,
                                PRUint32 aFlags) = 0;

  void RenderTo(const nsIntPoint& aOffset, LayerManagerOGL* aManager,
                PRUint32 aFlags);

  nsIntSize GetSize() {
    if (mTexImage)
      return mTexImage->GetSize();
    return nsIntSize(0, 0);
  }

protected:
  virtual nsIntPoint GetOriginOffset() = 0;

  GLContext* gl() const { return mOGLLayer->gl(); }

  ThebesLayer* mLayer;
  LayerOGL* mOGLLayer;
  nsRefPtr<TextureImage> mTexImage;
  nsRefPtr<TextureImage> mTexImageOnWhite;
};

void
ThebesLayerBufferOGL::RenderTo(const nsIntPoint& aOffset,
                               LayerManagerOGL* aManager,
                               PRUint32 aFlags)
{
  if (!mTexImage)
    return;

  if (mTexImage->InUpdate()) {
    mTexImage->EndUpdate();
  }

  if (mTexImageOnWhite && mTexImageOnWhite->InUpdate()) {
    mTexImageOnWhite->EndUpdate();
  }

  // Bind textures.
  TextureImage::ScopedBindTexture texBind(mTexImage, LOCAL_GL_TEXTURE0);
  TextureImage::ScopedBindTexture texOnWhiteBind(mTexImageOnWhite, LOCAL_GL_TEXTURE1);

  PRInt32 passes = mTexImageOnWhite ? 2 : 1;
  for (PRInt32 pass = 1; pass <= passes; ++pass) {
    LayerProgram *program;

    if (passes == 2) {
      ComponentAlphaTextureLayerProgram *alphaProgram;
      NS_ASSERTION(!mTexImage->IsRGB() && !mTexImageOnWhite->IsRGB(),
                   "Only BGR image surported with component alpha (currently!)");
      if (pass == 1) {
        alphaProgram = aManager->GetComponentAlphaPass1LayerProgram();
        gl()->fBlendFuncSeparate(LOCAL_GL_ZERO, LOCAL_GL_ONE_MINUS_SRC_COLOR,
                                 LOCAL_GL_ONE, LOCAL_GL_ONE);
      } else {
        alphaProgram = aManager->GetComponentAlphaPass2LayerProgram();
        gl()->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE,
                                 LOCAL_GL_ONE, LOCAL_GL_ONE);
      }

      alphaProgram->Activate();
      alphaProgram->SetBlackTextureUnit(0);
      alphaProgram->SetWhiteTextureUnit(1);
      program = alphaProgram;
    } else {
      // Note BGR: Cairo's image surfaces are always in what
      // OpenGL and our shaders consider BGR format.
      ColorTextureLayerProgram *basicProgram =
        aManager->GetColorTextureLayerProgram(mTexImage->GetShaderProgramType());

      basicProgram->Activate();
      basicProgram->SetTextureUnit(0);
      program = basicProgram;
    }

    program->SetLayerOpacity(mLayer->GetEffectiveOpacity());
    program->SetLayerTransform(mLayer->GetEffectiveTransform());
    program->SetRenderOffset(aOffset);

    const nsIntRegion& visibleRegion = mLayer->GetEffectiveVisibleRegion();
    nsIntRegion tmpRegion;
    const nsIntRegion* renderRegion;
    if (aFlags & PAINT_WILL_RESAMPLE) {
      // If we're resampling, then the texture image will contain exactly the
      // entire visible region's bounds, and we should draw it all in one quad
      // to avoid unexpected aliasing.
      tmpRegion = visibleRegion.GetBounds();
      renderRegion = &tmpRegion;
    } else {
      renderRegion = &visibleRegion;
    }
    nsIntRegionRectIterator iter(*renderRegion);
    while (const nsIntRect *iterRect = iter.Next()) {
      nsIntRect quadRect = *iterRect;
      program->SetLayerQuadRect(quadRect);

      quadRect.MoveBy(-GetOriginOffset());

      BindAndDrawQuadWithTextureRect(gl(), program, quadRect,
                                     mTexImage->GetSize(),
                                     mTexImage->GetWrapMode());
    }
  }

  if (mTexImageOnWhite) {
    // Restore defaults
    gl()->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA,
                             LOCAL_GL_ONE, LOCAL_GL_ONE);
  }
}


// This implementation is the fast-path for when our TextureImage is
// permanently backed with a server-side ASurface.  We can simply
// reuse the ThebesLayerBuffer logic in its entirety and profit.
class SurfaceBufferOGL : public ThebesLayerBufferOGL, private ThebesLayerBuffer
{
public:
  typedef ThebesLayerBufferOGL::ContentType ContentType;
  typedef ThebesLayerBufferOGL::PaintState PaintState;

  SurfaceBufferOGL(ThebesLayerOGL* aLayer)
    : ThebesLayerBufferOGL(aLayer, aLayer)
    , ThebesLayerBuffer(SizedToVisibleBounds)
  {
  }
  virtual ~SurfaceBufferOGL() {}

  // ThebesLayerBufferOGL interface
  virtual PaintState BeginPaint(ContentType aContentType, 
                                PRUint32 aFlags)
  {
    // Let ThebesLayerBuffer do all the hard work for us! :D
    return ThebesLayerBuffer::BeginPaint(mLayer, 
                                         aContentType, 
                                         aFlags);
  }

  // ThebesLayerBuffer interface
  virtual already_AddRefed<gfxASurface>
  CreateBuffer(ContentType aType, const nsIntSize& aSize, PRUint32 aFlags)
  {
    NS_ASSERTION(gfxASurface::CONTENT_ALPHA != aType,"ThebesBuffer has color");

    mTexImage = CreateClampOrRepeatTextureImage(gl(), aSize, aType, aFlags);
    return mTexImage ? mTexImage->GetBackingSurface() : nsnull;
  }

protected:
  virtual nsIntPoint GetOriginOffset() {
    return BufferRect().TopLeft() - BufferRotation();
  }
};


// This implementation is (currently) the slow-path for when we can't
// implement pixel retaining using thebes.  This implementation and
// the above could be unified by abstracting buffer-copy operations
// and implementing them here using GL hacketry.
class BasicBufferOGL : public ThebesLayerBufferOGL
{
public:
  BasicBufferOGL(ThebesLayerOGL* aLayer)
    : ThebesLayerBufferOGL(aLayer, aLayer)
    , mBufferRect(0,0,0,0)
    , mBufferRotation(0,0)
  {}
  virtual ~BasicBufferOGL() {}

  virtual PaintState BeginPaint(ContentType aContentType,
                                PRUint32 aFlags);

protected:
  enum XSide {
    LEFT, RIGHT
  };
  enum YSide {
    TOP, BOTTOM
  };
  nsIntRect GetQuadrantRectangle(XSide aXSide, YSide aYSide);

  virtual nsIntPoint GetOriginOffset() {
    return mBufferRect.TopLeft() - mBufferRotation;
  }

private:
  nsIntRect mBufferRect;
  nsIntPoint mBufferRotation;
};

static void
WrapRotationAxis(PRInt32* aRotationPoint, PRInt32 aSize)
{
  if (*aRotationPoint < 0) {
    *aRotationPoint += aSize;
  } else if (*aRotationPoint >= aSize) {
    *aRotationPoint -= aSize;
  }
}

nsIntRect
BasicBufferOGL::GetQuadrantRectangle(XSide aXSide, YSide aYSide)
{
  // quadrantTranslation is the amount we translate the top-left
  // of the quadrant by to get coordinates relative to the layer
  nsIntPoint quadrantTranslation = -mBufferRotation;
  quadrantTranslation.x += aXSide == LEFT ? mBufferRect.width : 0;
  quadrantTranslation.y += aYSide == TOP ? mBufferRect.height : 0;
  return mBufferRect + quadrantTranslation;
}

static void
FillSurface(gfxASurface* aSurface, const nsIntRegion& aRegion,
            const nsIntPoint& aOffset, const gfxRGBA& aColor)
{
  nsRefPtr<gfxContext> ctx = new gfxContext(aSurface);
  ctx->Translate(-gfxPoint(aOffset.x, aOffset.y));
  gfxUtils::ClipToRegion(ctx, aRegion);
  ctx->SetColor(aColor);
  ctx->Paint();
}

BasicBufferOGL::PaintState
BasicBufferOGL::BeginPaint(ContentType aContentType,
                           PRUint32 aFlags)
{
  PaintState result;
  // We need to disable rotation if we're going to be resampled when
  // drawing, because we might sample across the rotation boundary.
  PRBool canHaveRotation =  !(aFlags & PAINT_WILL_RESAMPLE);

  nsIntRegion validRegion = mLayer->GetValidRegion();

  Layer::SurfaceMode mode;
  ContentType contentType;
  nsIntRegion neededRegion;
  PRBool canReuseBuffer;
  nsIntRect destBufferRect;

  while (PR_TRUE) {
    mode = mLayer->GetSurfaceMode();
    contentType = aContentType;
    neededRegion = mLayer->GetVisibleRegion();
    // If we're going to resample, we need a buffer that's in clamp mode.
    canReuseBuffer = neededRegion.GetBounds().Size() <= mBufferRect.Size() &&
      mTexImage &&
      (!(aFlags & PAINT_WILL_RESAMPLE) ||
       mTexImage->GetWrapMode() == LOCAL_GL_CLAMP_TO_EDGE);

    if (canReuseBuffer) {
      if (mBufferRect.Contains(neededRegion.GetBounds())) {
        // We don't need to adjust mBufferRect.
        destBufferRect = mBufferRect;
      } else {
        // The buffer's big enough but doesn't contain everything that's
        // going to be visible. We'll move it.
        destBufferRect = nsIntRect(neededRegion.GetBounds().TopLeft(), mBufferRect.Size());
      }
    } else {
      destBufferRect = neededRegion.GetBounds();
    }

    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
#ifdef MOZ_GFX_OPTIMIZE_MOBILE
      mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
#else
      if (!mLayer->GetParent() || !mLayer->GetParent()->SupportsComponentAlphaChildren()) {
        mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
      } else {
        contentType = gfxASurface::CONTENT_COLOR;
      }
 #endif
    }
 
    if ((aFlags & PAINT_WILL_RESAMPLE) &&
        (!neededRegion.GetBounds().IsEqualInterior(destBufferRect) ||
         neededRegion.GetNumRects() > 1)) {
      // The area we add to neededRegion might not be painted opaquely
      if (mode == Layer::SURFACE_OPAQUE) {
        contentType = gfxASurface::CONTENT_COLOR_ALPHA;
        mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
      }
      // For component alpha layers, we leave contentType as CONTENT_COLOR.

      // We need to validate the entire buffer, to make sure that only valid
      // pixels are sampled
      neededRegion = destBufferRect;
    }

    if (mTexImage &&
        (mTexImage->GetContentType() != contentType ||
         (mode == Layer::SURFACE_COMPONENT_ALPHA) != (mTexImageOnWhite != nsnull))) {
      // We're effectively clearing the valid region, so we need to draw
      // the entire needed region now.
      result.mRegionToInvalidate = mLayer->GetValidRegion();
      validRegion.SetEmpty();
      mTexImage = nsnull;
      mTexImageOnWhite = nsnull;
      mBufferRect.SetRect(0, 0, 0, 0);
      mBufferRotation.MoveTo(0, 0);
      // Restart decision process with the cleared buffer. We can only go
      // around the loop one more iteration, since mTexImage is null now.
      continue;
    }

    break;
  }

  result.mRegionToDraw.Sub(neededRegion, validRegion);
  if (result.mRegionToDraw.IsEmpty())
    return result;

  if (destBufferRect.width > gl()->GetMaxTextureSize() ||
      destBufferRect.height > gl()->GetMaxTextureSize()) {
    return result;
  }

  nsIntRect drawBounds = result.mRegionToDraw.GetBounds();
  nsRefPtr<TextureImage> destBuffer;
  nsRefPtr<TextureImage> destBufferOnWhite;

  PRUint32 bufferFlags = canHaveRotation ? ALLOW_REPEAT : 0;
  if (canReuseBuffer) {
    nsIntRect keepArea;
    if (keepArea.IntersectRect(destBufferRect, mBufferRect)) {
      // Set mBufferRotation so that the pixels currently in mBuffer
      // will still be rendered in the right place when mBufferRect
      // changes to destBufferRect.
      nsIntPoint newRotation = mBufferRotation +
        (destBufferRect.TopLeft() - mBufferRect.TopLeft());
      WrapRotationAxis(&newRotation.x, mBufferRect.width);
      WrapRotationAxis(&newRotation.y, mBufferRect.height);
      NS_ASSERTION(nsIntRect(nsIntPoint(0,0), mBufferRect.Size()).Contains(newRotation),
                   "newRotation out of bounds");
      PRInt32 xBoundary = destBufferRect.XMost() - newRotation.x;
      PRInt32 yBoundary = destBufferRect.YMost() - newRotation.y;
      if ((drawBounds.x < xBoundary && xBoundary < drawBounds.XMost()) ||
          (drawBounds.y < yBoundary && yBoundary < drawBounds.YMost()) ||
          (newRotation != nsIntPoint(0,0) && !canHaveRotation)) {
        // The stuff we need to redraw will wrap around an edge of the
        // buffer, so we will need to do a self-copy
        // If mBufferRotation == nsIntPoint(0,0) we could do a real
        // self-copy but we're not going to do that in GL yet.
        // We can't do a real self-copy because the buffer is rotated.
        // So allocate a new buffer for the destination.
        destBufferRect = neededRegion.GetBounds();
        destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        if (!destBuffer)
          return result;
        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite =
            CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
          if (!destBufferOnWhite)
            return result;
        }
      } else {
        mBufferRect = destBufferRect;
        mBufferRotation = newRotation;
      }
    } else {
      // No pixels are going to be kept. The whole visible region
      // will be redrawn, so we don't need to copy anything, so we don't
      // set destBuffer.
      mBufferRect = destBufferRect;
      mBufferRotation = nsIntPoint(0,0);
    }
  } else {
    // The buffer's not big enough, so allocate a new one
    destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
    if (!destBuffer)
      return result;

    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
      destBufferOnWhite = 
        CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
      if (!destBufferOnWhite)
        return result;
    }
  }
  NS_ASSERTION(!(aFlags & PAINT_WILL_RESAMPLE) || destBufferRect == neededRegion.GetBounds(),
               "If we're resampling, we need to validate the entire buffer");

  if (!destBuffer && !mTexImage) {
    return result;
  }

  if (destBuffer) {
    if (mTexImage && (mode != Layer::SURFACE_COMPONENT_ALPHA || mTexImageOnWhite)) {
      // BlitTextureImage depends on the FBO texture target being
      // TEXTURE_2D.  This isn't the case on some older X1600-era Radeons.
      if (mOGLLayer->OGLManager()->FBOTextureTarget() == LOCAL_GL_TEXTURE_2D) {
        nsIntRect overlap;
        overlap.IntersectRect(mBufferRect, destBufferRect);

        nsIntRect srcRect(overlap), dstRect(overlap);
        srcRect.MoveBy(- mBufferRect.TopLeft() + mBufferRotation);
        dstRect.MoveBy(- destBufferRect.TopLeft());

        destBuffer->Resize(destBufferRect.Size());

        gl()->BlitTextureImage(mTexImage, srcRect,
                               destBuffer, dstRect);
        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite->Resize(destBufferRect.Size());
          gl()->BlitTextureImage(mTexImageOnWhite, srcRect,
                                 destBufferOnWhite, dstRect);
        }
      } else {
        // can't blit, just draw everything
        destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite = 
            CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        }
      }
    }

    mTexImage = destBuffer.forget();
    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
      mTexImageOnWhite = destBufferOnWhite.forget();
    }
    mBufferRect = destBufferRect;
    mBufferRotation = nsIntPoint(0,0);
  }
  NS_ASSERTION(canHaveRotation || mBufferRotation == nsIntPoint(0,0),
               "Rotation disabled, but we have nonzero rotation?");

  nsIntRegion invalidate;
  invalidate.Sub(mLayer->GetValidRegion(), destBufferRect);
  result.mRegionToInvalidate.Or(result.mRegionToInvalidate, invalidate);

  // Figure out which quadrant to draw in
  PRInt32 xBoundary = mBufferRect.XMost() - mBufferRotation.x;
  PRInt32 yBoundary = mBufferRect.YMost() - mBufferRotation.y;
  XSide sideX = drawBounds.XMost() <= xBoundary ? RIGHT : LEFT;
  YSide sideY = drawBounds.YMost() <= yBoundary ? BOTTOM : TOP;
  nsIntRect quadrantRect = GetQuadrantRectangle(sideX, sideY);
  NS_ASSERTION(quadrantRect.Contains(drawBounds), "Messed up quadrants");

  nsIntPoint offset = -nsIntPoint(quadrantRect.x, quadrantRect.y);

  // Make the region to draw relative to the buffer, before
  // passing to BeginUpdate.
  result.mRegionToDraw.MoveBy(offset);
  // BeginUpdate is allowed to modify the given region,
  // if it wants more to be repainted than we request.
  if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
    nsIntRegion drawRegionCopy = result.mRegionToDraw;
    gfxASurface *onBlack = mTexImage->BeginUpdate(drawRegionCopy);
    gfxASurface *onWhite = mTexImageOnWhite->BeginUpdate(result.mRegionToDraw);
    NS_ASSERTION(result.mRegionToDraw == drawRegionCopy,
                 "BeginUpdate should always modify the draw region in the same way!");
    FillSurface(onBlack, result.mRegionToDraw, nsIntPoint(0,0), gfxRGBA(0.0, 0.0, 0.0, 1.0));
    FillSurface(onWhite, result.mRegionToDraw, nsIntPoint(0,0), gfxRGBA(1.0, 1.0, 1.0, 1.0));
    gfxASurface* surfaces[2] = { onBlack, onWhite };
    nsRefPtr<gfxTeeSurface> surf = new gfxTeeSurface(surfaces, NS_ARRAY_LENGTH(surfaces));

    // XXX If the device offset is set on the individual surfaces instead of on
    // the tee surface, we render in the wrong place. Why?
    gfxPoint deviceOffset = onBlack->GetDeviceOffset();
    onBlack->SetDeviceOffset(gfxPoint(0, 0));
    onWhite->SetDeviceOffset(gfxPoint(0, 0));
    surf->SetDeviceOffset(deviceOffset);

    // Using this surface as a source will likely go horribly wrong, since
    // only the onBlack surface will really be used, so alpha information will
    // be incorrect.
    surf->SetAllowUseAsSource(PR_FALSE);
    result.mContext = new gfxContext(surf);
  } else {
    result.mContext = new gfxContext(mTexImage->BeginUpdate(result.mRegionToDraw));
    if (mTexImage->GetContentType() == gfxASurface::CONTENT_COLOR_ALPHA) {
      gfxUtils::ClipToRegion(result.mContext, result.mRegionToDraw);
      result.mContext->SetOperator(gfxContext::OPERATOR_CLEAR);
      result.mContext->Paint();
      result.mContext->SetOperator(gfxContext::OPERATOR_OVER);
    }
  }
  if (!result.mContext) {
    NS_WARNING("unable to get context for update");
    return result;
  }
  result.mContext->Translate(-gfxPoint(quadrantRect.x, quadrantRect.y));
  // Move rgnToPaint back into position so that the thebes callback
  // gets the right coordintes.
  result.mRegionToDraw.MoveBy(-offset);
  
  return result;
}

ThebesLayerOGL::ThebesLayerOGL(LayerManagerOGL *aManager)
  : ThebesLayer(aManager, nsnull)
  , LayerOGL(aManager)
  , mBuffer(nsnull)
{
  mImplData = static_cast<LayerOGL*>(this);
}

ThebesLayerOGL::~ThebesLayerOGL()
{
  Destroy();
}

void
ThebesLayerOGL::Destroy()
{
  if (!mDestroyed) {
    mBuffer = nsnull;
    mDestroyed = PR_TRUE;
  }
}

PRBool
ThebesLayerOGL::CreateSurface()
{
  NS_ASSERTION(!mBuffer, "buffer already created?");

  if (mVisibleRegion.IsEmpty()) {
    return PR_FALSE;
  }

  if (gl()->TextureImageSupportsGetBackingSurface()) {
    // use the ThebesLayerBuffer fast-path
    mBuffer = new SurfaceBufferOGL(this);
  } else {
    mBuffer = new BasicBufferOGL(this);
  }
  return PR_TRUE;
}

void
ThebesLayerOGL::SetVisibleRegion(const nsIntRegion &aRegion)
{
  if (aRegion.IsEqual(mVisibleRegion))
    return;
  ThebesLayer::SetVisibleRegion(aRegion);
}

void
ThebesLayerOGL::InvalidateRegion(const nsIntRegion &aRegion)
{
  mValidRegion.Sub(mValidRegion, aRegion);
}

void
ThebesLayerOGL::RenderLayer(int aPreviousFrameBuffer,
                            const nsIntPoint& aOffset)
{
  if (!mBuffer && !CreateSurface()) {
    return;
  }
  NS_ABORT_IF_FALSE(mBuffer, "should have a buffer here");

  mOGLManager->MakeCurrent();
  gl()->fActiveTexture(LOCAL_GL_TEXTURE0);

  TextureImage::ContentType contentType =
    CanUseOpaqueSurface() ? gfxASurface::CONTENT_COLOR :
                            gfxASurface::CONTENT_COLOR_ALPHA;

  gfxMatrix transform2d;
  PRUint32 flags = 0;
  if (GetEffectiveTransform().Is2D(&transform2d)) {
    if (transform2d.HasNonIntegerTranslation()) {
      flags |= ThebesLayerBufferOGL::PAINT_WILL_RESAMPLE;
    }
  } else {
    flags |= ThebesLayerBufferOGL::PAINT_WILL_RESAMPLE;
  }

  Buffer::PaintState state = mBuffer->BeginPaint(contentType, flags);
  mValidRegion.Sub(mValidRegion, state.mRegionToInvalidate);

  if (state.mContext) {
    state.mRegionToInvalidate.And(state.mRegionToInvalidate, mVisibleRegion);

    LayerManager::DrawThebesLayerCallback callback =
      mOGLManager->GetThebesLayerCallback();
    if (!callback) {
      NS_ERROR("GL should never need to update ThebesLayers in an empty transaction");
    } else {
      void* callbackData = mOGLManager->GetThebesLayerCallbackData();
      SetAntialiasingFlags(this, state.mContext);
      callback(this, state.mContext, state.mRegionToDraw,
               state.mRegionToInvalidate, callbackData);
      // Everything that's visible has been validated. Do this instead of just
      // OR-ing with aRegionToDraw, since that can lead to a very complex region
      // here (OR doesn't automatically simplify to the simplest possible
      // representation of a region.)
      nsIntRegion tmp;
      tmp.Or(mVisibleRegion, state.mRegionToDraw);
      mValidRegion.Or(mValidRegion, tmp);
    }
  }

  // Drawing thebes layers can change the current context, reset it.
  gl()->MakeCurrent();

  gl()->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, aPreviousFrameBuffer);
  mBuffer->RenderTo(aOffset, mOGLManager, flags);
}

Layer*
ThebesLayerOGL::GetLayer()
{
  return this;
}

PRBool
ThebesLayerOGL::IsEmpty()
{
  return !mBuffer;
}


class ShadowBufferOGL : public ThebesLayerBufferOGL
{
public:
  ShadowBufferOGL(ShadowThebesLayerOGL* aLayer)
    : ThebesLayerBufferOGL(aLayer, aLayer)
  {}

  virtual PaintState BeginPaint(ContentType aContentType, PRUint32) {
    NS_RUNTIMEABORT("can't BeginPaint for a shadow layer");
    return PaintState();
  }

  void Upload(gfxASurface* aUpdate, const nsIntRegion& aUpdated,
              const nsIntRect& aRect, const nsIntPoint& aRotation);

protected:
  virtual nsIntPoint GetOriginOffset() {
    return mBufferRect.TopLeft() - mBufferRotation;
  }

private:
  nsIntRect mBufferRect;
  nsIntPoint mBufferRotation;
};

void
ShadowBufferOGL::Upload(gfxASurface* aUpdate, const nsIntRegion& aUpdated,
                        const nsIntRect& aRect, const nsIntPoint& aRotation)
{
  gfxIntSize size = aUpdate->GetSize();
  if (GetSize() != nsIntSize(size.width, size.height)) {
    // XXX we should do something here to decide whether to use REPEAT or not,
    // but I'm not sure what
    mTexImage = CreateClampOrRepeatTextureImage(gl(),
      nsIntSize(size.width, size.height), aUpdate->GetContentType(), ALLOW_REPEAT);
  }

  nsIntRegion destRegion(aUpdated);
  // aUpdated is in screen coordinates.  Move it so that the layer's
  // top-left is 0,0
  nsIntPoint visTopLeft = mLayer->GetVisibleRegion().GetBounds().TopLeft();
  destRegion.MoveBy(-visTopLeft);

  // NB: this gfxContext must not escape EndUpdate() below
  mTexImage->DirectUpdate(aUpdate, destRegion);

  mBufferRect = aRect;
  mBufferRotation = aRotation;
}

ShadowThebesLayerOGL::ShadowThebesLayerOGL(LayerManagerOGL *aManager)
  : ShadowThebesLayer(aManager, nsnull)
  , LayerOGL(aManager)
{
  mImplData = static_cast<LayerOGL*>(this);
}

ShadowThebesLayerOGL::~ShadowThebesLayerOGL()
{}

void
ShadowThebesLayerOGL::SetFrontBuffer(const OptionalThebesBuffer& aNewFront,
                                     const nsIntRegion& aValidRegion)
{
  if (mDestroyed) {
    return;
  }

  if (!mBuffer) {
    mBuffer = new ShadowBufferOGL(this);
  }

  NS_ASSERTION(OptionalThebesBuffer::Tnull_t == aNewFront.type(),
               "Only one system-memory buffer expected");
}

void
ShadowThebesLayerOGL::Swap(const ThebesBuffer& aNewFront,
                           const nsIntRegion& aUpdatedRegion,
                           ThebesBuffer* aNewBack,
                           nsIntRegion* aNewBackValidRegion,
                           OptionalThebesBuffer* aReadOnlyFront,
                           nsIntRegion* aFrontUpdatedRegion)
{
  if (!mDestroyed && mBuffer) {
    nsRefPtr<gfxASurface> surf = ShadowLayerForwarder::OpenDescriptor(aNewFront.buffer());
    mBuffer->Upload(surf, aUpdatedRegion, aNewFront.rect(), aNewFront.rotation());
  }

  *aNewBack = aNewFront;
  *aNewBackValidRegion = mValidRegion;
  *aReadOnlyFront = null_t();
  aFrontUpdatedRegion->SetEmpty();
}

void
ShadowThebesLayerOGL::DestroyFrontBuffer()
{
  mBuffer = nsnull;
}

void
ShadowThebesLayerOGL::Disconnect()
{
  Destroy();
}

void
ShadowThebesLayerOGL::Destroy()
{
  if (!mDestroyed) {
    mDestroyed = PR_TRUE;
    mBuffer = nsnull;
  }
}

Layer*
ShadowThebesLayerOGL::GetLayer()
{
  return this;
}

PRBool
ShadowThebesLayerOGL::IsEmpty()
{
  return !mBuffer;
}

void
ShadowThebesLayerOGL::RenderLayer(int aPreviousFrameBuffer,
                                  const nsIntPoint& aOffset)
{
  if (!mBuffer) {
    return;
  }
  NS_ABORT_IF_FALSE(mBuffer, "should have a buffer here");

  mOGLManager->MakeCurrent();
  gl()->fActiveTexture(LOCAL_GL_TEXTURE0);

  gl()->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, aPreviousFrameBuffer);
  mBuffer->RenderTo(aOffset, mOGLManager, 0);
}

} /* layers */
} /* mozilla */
