#ifndef CIVUE_METAL_H
#define CIVUE_METAL_H

#include <civue/objc.h>

#ifdef __cplusplus
extern "C" {
#endif

// values and layouts of the metal sdk, for metal from c without objective-c headers.
enum : NSUInteger {
  MTLPixelFormatRGBA8Unorm = 70,
  MTLPixelFormatRGBA16Unorm = 110,
  MTLPixelFormatStencil8 = 253,
  MTLTextureType2D = 2,
  MTLTextureType2DMultisample = 4,
  MTLStorageModeShared = 0,
  MTLStorageModePrivate = 2,
  MTLStorageModeMemoryless = 3,
  MTLResourceStorageModeShared = 0,
  MTLTextureUsageShaderRead = 1,
  MTLTextureUsageRenderTarget = 4,
  MTLCompareFunctionEqual = 2,
  MTLCompareFunctionNotEqual = 5,
  MTLCompareFunctionAlways = 7,
  MTLStencilOperationKeep = 0,
  MTLStencilOperationZero = 1,
  MTLStencilOperationReplace = 2,
  MTLStencilOperationInvert = 5,
  MTLStencilOperationIncrementWrap = 6,
  MTLStencilOperationDecrementWrap = 7,
  MTLBlendFactorZero = 0,
  MTLBlendFactorOne = 1,
  MTLBlendFactorOneMinusSourceColor = 3,
  MTLBlendFactorOneMinusSourceAlpha = 5,
  MTLBlendFactorBlendColor = 11,
  MTLBlendFactorOneMinusBlendColor = 12,
  MTLBlendFactorBlendAlpha = 13,
  MTLBlendFactorOneMinusBlendAlpha = 14,
  MTLBlendOperationAdd = 0,
  MTLColorWriteMaskNone = 0,
  MTLColorWriteMaskAll = 15,
  MTLVertexFormatFloat2 = 29,
  MTLVertexStepFunctionPerVertex = 1,
  MTLPrimitiveTypeTriangle = 3,
  MTLWindingCounterClockwise = 1,
  MTLLoadActionDontCare = 0,
  MTLLoadActionLoad = 1,
  MTLLoadActionClear = 2,
  MTLStoreActionDontCare = 0,
  MTLStoreActionStore = 1,
  MTLDataTypeInt = 29,
  MTLPurgeableStateEmpty = 4,
  MTLSamplerMinMagFilterLinear = 1,
  MTLSamplerMipFilterLinear = 2,
  MTLSamplerAddressModeClampToEdge = 0,
  MTLSamplerAddressModeRepeat = 2,
  MTLLanguageVersion3_1 = 3 << 16 | 1,
};
enum : NSInteger { MTLGPUFamilyApple1 = 1001 };
// MTLCommandBufferError codes.
enum : NSInteger {
  MTLCommandBufferErrorAccessRevoked = 4,
  MTLCommandBufferErrorDeviceRemoved = 11,
};
typedef struct {
  NSUInteger x, y, z;
} MTLOrigin;
typedef struct {
  NSUInteger width, height, depth;
} MTLSize;
typedef struct {
  MTLOrigin origin;
  MTLSize size;
} MTLRegion;
typedef struct {
  double red, green, blue, alpha;
} MTLClearColor;
typedef struct {
  NSUInteger x, y, width, height;
} MTLScissorRect;
typedef struct {
  double originX, originY, width, height, znear, zfar;
} MTLViewport;
id MTLCreateSystemDefaultDevice(void);

#ifdef __cplusplus
}
#endif

#endif
