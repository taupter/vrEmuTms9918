/*
 * Troy's TMS9918 Emulator - Core interface
 *
 * Copyright (c) 2021 Troy Schrapel
 *
 * This code is licensed under the MIT license
 *
 * https://github.com/visrealm/VrEmuTms9918
 *
 */

#include "vrEmuTms9918.h"
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include <string.h>

#if PICO_BUILD
#include "pico/stdlib.h"
#define inline __force_inline
#else
#define __time_critical_func(fn) fn
#endif

#ifndef WIN32
#undef VR_EMU_TMS9918_DLLEXPORT
#define VR_EMU_TMS9918_DLLEXPORT
#endif


#define VRAM_SIZE           (1 << 14) /* 16KB */
#define VRAM_MASK     (VRAM_SIZE - 1) /* 0x3fff */

#define GRAPHICS_NUM_COLS         32
#define GRAPHICS_NUM_ROWS         24
#define GRAPHICS_CHAR_WIDTH        8

#define TEXT_NUM_COLS             40
#define TEXT_NUM_ROWS             24
#define TEXT_CHAR_WIDTH            6
#define TEXT_PADDING_PX            8
#define TEXT80_NUM_COLS           80

#define PATTERN_BYTES              8
#define GFXI_COLOR_GROUP_SIZE      8

#define MAX_SPRITES               32

#define SPRITE_ATTR_Y              0
#define SPRITE_ATTR_X              1
#define SPRITE_ATTR_NAME           2
#define SPRITE_ATTR_COLOR          3
#define SPRITE_ATTR_BYTES          4
#define LAST_SPRITE_YPOS        0xD0
#define MAX_SCANLINE_SPRITES       4

#define STATUS_INT              0x80
#define STATUS_5S               0x40
#define STATUS_COL              0x20

#define TMS_R0_MODE_TEXT_80     0x04
#define TMS_R0_MODE_GRAPHICS_II 0x02
#define TMS_R0_EXT_VDP_ENABLE   0x01

#define TMS_R1_DISP_ACTIVE      0x40
#define TMS_R1_INT_ENABLE       0x20
#define TMS_R1_MODE_MULTICOLOR  0x08
#define TMS_R1_MODE_TEXT        0x10
#define TMS_R1_SPRITE_16        0x02
#define TMS_R1_SPRITE_MAG2      0x01

 /* PRIVATE DATA STRUCTURE
  * ---------------------- */
struct vrEmuTMS9918_s
{
  /* the eight write-only registers */
  uint8_t registers[TMS_NUM_REGISTERS];

  /* status register (read-only) */
  uint8_t status;

  /* current address for cpu access (auto-increments) */
  uint16_t currentAddress;

  /* address or register write stage (0 or 1) */
  uint8_t regWriteStage;

  /* holds first stage of write to address/register port */
  uint8_t regWriteStage0Value;

  /* buffered value */
  uint8_t readAheadBuffer;

  bool interruptsEnabled;

  /* current display mode */
  vrEmuTms9918Mode mode;

  /* video ram */
  uint8_t vram[VRAM_SIZE];

  uint8_t rowSpriteBits[TMS9918_PIXELS_X]; /* collision mask */

  bool pattLookupInit;
  uint32_t pattLookup[256 * 16];
};


/* Function:  tmsMode
 * ----------------------------------------
 * return the current display mode
 */
static vrEmuTms9918Mode __time_critical_func(tmsMode)(VrEmuTms9918* tms9918)
{
  tms9918->interruptsEnabled = tms9918->registers[TMS_REG_1] & TMS_R1_INT_ENABLE;

  if (tms9918->registers[TMS_REG_0] & TMS_R0_MODE_GRAPHICS_II)
  {
    return TMS_MODE_GRAPHICS_II;
  }

  /* MC and TEX bits 3 and 4. Shift to bits 0 and 1 to determine a value (0, 1 or 2) */
  switch ((tms9918->registers[TMS_REG_1] & (TMS_R1_MODE_MULTICOLOR | TMS_R1_MODE_TEXT)) >> 3)
  {
    case 0:
      return TMS_MODE_GRAPHICS_I;

    case 1:
      return TMS_MODE_MULTICOLOR;

    case 2:
      return tms9918->registers[TMS_REG_0] & TMS_R0_MODE_TEXT_80 ? TMS_MODE_TEXT80 : TMS_MODE_TEXT;
  }
  return TMS_MODE_GRAPHICS_I;
}


/* Function:  tmsSpriteSize
 * ----------------------------------------
 * sprite size (8 or 16)
 */
static inline uint8_t tmsSpriteSize(VrEmuTms9918* tms9918)
{
  return tms9918->registers[TMS_REG_1] & TMS_R1_SPRITE_16 ? 16 : 8;
}

/* Function:  tmsSpriteMagnification
 * ----------------------------------------
 * sprite size (0 = 1x, 1 = 2x)
 */
static inline bool tmsSpriteMag(VrEmuTms9918* tms9918)
{
  return tms9918->registers[TMS_REG_1] & TMS_R1_SPRITE_MAG2;
}

/* Function:  tmsNameTableAddr
 * ----------------------------------------
 * name table base address
 */
static inline uint16_t tmsNameTableAddr(VrEmuTms9918* tms9918)
{
  return (tms9918->registers[TMS_REG_NAME_TABLE] & 0x0f) << 10;
}

/* Function:  tmsColorTableAddr
 * ----------------------------------------
 * color table base address
 */
static inline uint16_t tmsColorTableAddr(VrEmuTms9918* tms9918)
{
  const uint8_t mask = (tms9918->mode == TMS_MODE_GRAPHICS_II) ? 0x80 : 0xff;

  return (tms9918->registers[TMS_REG_COLOR_TABLE] & mask) << 6;
}

/* Function:  tmsPatternTableAddr
 * ----------------------------------------
 * pattern table base address
 */
static inline uint16_t tmsPatternTableAddr(VrEmuTms9918* tms9918)
{
  const uint8_t mask = (tms9918->mode == TMS_MODE_GRAPHICS_II) ? 0x04 : 0x07;

  return (tms9918->registers[TMS_REG_PATTERN_TABLE] & mask) << 11;
}

/* Function:  tmsSpriteAttrTableAddr
 * ----------------------------------------
 * sprite attribute table base address
 */
static inline uint16_t tmsSpriteAttrTableAddr(VrEmuTms9918* tms9918)
{
  return (tms9918->registers[TMS_REG_SPRITE_ATTR_TABLE] & 0x7f) << 7;
}

/* Function:  tmsSpritePatternTableAddr
 * ----------------------------------------
 * sprite pattern table base address
 */
static inline uint16_t tmsSpritePatternTableAddr(VrEmuTms9918* tms9918)
{
  return (tms9918->registers[TMS_REG_SPRITE_PATT_TABLE] & 0x07) << 11;
}

/* Function:  tmsBgColor
 * ----------------------------------------
 * background color
 */
static inline vrEmuTms9918Color tmsMainBgColor(VrEmuTms9918* tms9918)
{
  return tms9918->registers[TMS_REG_FG_BG_COLOR] & 0x0f;
}

/* Function:  tmsFgColor
 * ----------------------------------------
 * foreground color
 */
static inline vrEmuTms9918Color tmsMainFgColor(VrEmuTms9918* tms9918)
{
  const vrEmuTms9918Color c = (vrEmuTms9918Color)(tms9918->registers[TMS_REG_FG_BG_COLOR] >> 4);
  return c == TMS_TRANSPARENT ? tmsMainBgColor(tms9918) : c;
}

/* Function:  tmsFgColor
 * ----------------------------------------
 * foreground color
 */
static inline vrEmuTms9918Color tmsFgColor(VrEmuTms9918* tms9918, uint8_t colorByte)
{
  const vrEmuTms9918Color c = (vrEmuTms9918Color)(colorByte >> 4);
  return c == TMS_TRANSPARENT ? tmsMainBgColor(tms9918) : c;
}

/* Function:  tmsBgColor
 * ----------------------------------------
 * background color
 */
static inline vrEmuTms9918Color tmsBgColor(VrEmuTms9918* tms9918, uint8_t colorByte)
{
  const vrEmuTms9918Color c = (vrEmuTms9918Color)(colorByte & 0x0f);
  return c == TMS_TRANSPARENT ? tmsMainBgColor(tms9918) : c;
}


/* Function:  vrEmuTms9918New
 * ----------------------------------------
 * create a new TMS9918
 */
VR_EMU_TMS9918_DLLEXPORT VrEmuTms9918* vrEmuTms9918New()
{
  VrEmuTms9918* tms9918 = (VrEmuTms9918*)malloc(sizeof(VrEmuTms9918));
  if (tms9918 != NULL)
  {
    vrEmuTms9918Reset(tms9918);
  }

  return tms9918;
}

/* Function:  vrEmuTms9918Reset
 * ----------------------------------------
 * reset the new TMS9918
 */
VR_EMU_TMS9918_DLLEXPORT void vrEmuTms9918Reset(VrEmuTms9918* tms9918)
{
  tms9918->regWriteStage0Value = 0;
  tms9918->currentAddress = 0;
  tms9918->regWriteStage = 0;
  tms9918->status = 0;
  tms9918->readAheadBuffer = 0;
  memset(tms9918->registers, 0, sizeof(tms9918->registers));
  tms9918->pattLookupInit = false;

  /* ram intentionally left in unknown state */

  tms9918->mode = tmsMode(tms9918);
}


/* Function:  vrEmuTms9918Destroy
 * ----------------------------------------
 * destroy a TMS9918
 *
 * tms9918: tms9918 object to destroy / clean up
 */
VR_EMU_TMS9918_DLLEXPORT void vrEmuTms9918Destroy(VrEmuTms9918* tms9918)
{
  free(tms9918);
}

/* Function:  vrEmuTms9918WriteAddr
 * ----------------------------------------
 * write an address (mode = 1) to the tms9918
 *
 * data: the data (DB0 -> DB7) to send
 */
VR_EMU_TMS9918_DLLEXPORT void __time_critical_func(vrEmuTms9918WriteAddr)(VrEmuTms9918* tms9918, uint8_t data)
{
  if (tms9918->regWriteStage == 0)
  {
    /* first stage byte - either an address LSB or a register value */

    tms9918->regWriteStage0Value = data;
    tms9918->regWriteStage = 1;
  }
  else
  {
    /* second byte - either a register number or an address MSB */

    if (data & 0x80) /* register */
    {
      if ((data & 0x78) == 0)
      {
        int regIndex = data & 0x07;
        tms9918->registers[regIndex] = tms9918->regWriteStage0Value;
        if (regIndex < 2)
        {
          tms9918->mode = tmsMode(tms9918);
        }
      }
      else
      {
        // ignore higher registers
      }
    }
    else /* address */
    {
      tms9918->currentAddress = tms9918->regWriteStage0Value | ((data & 0x3f) << 8);
      if ((data & 0x40) == 0)
      {
        tms9918->readAheadBuffer = tms9918->vram[(tms9918->currentAddress++) & VRAM_MASK];
      }
    }
    tms9918->regWriteStage = 0;
  }
}

/* Function:  vrEmuTms9918ReadStatus
 * ----------------------------------------
 * read from the status register
 */
VR_EMU_TMS9918_DLLEXPORT uint8_t __time_critical_func(vrEmuTms9918ReadStatus)(VrEmuTms9918* tms9918)
{
  const uint8_t tmpStatus = tms9918->status;
  tms9918->status = 0;
  tms9918->regWriteStage = 0;
  return tmpStatus;
}

/* Function:  vrEmuTms9918PeekStatus
 * ----------------------------------------
 * read from the status register without resetting it
 */
VR_EMU_TMS9918_DLLEXPORT uint8_t __time_critical_func(vrEmuTms9918PeekStatus)(VrEmuTms9918* tms9918)
{
  return tms9918->status;
}

/* Function:  vrEmuTms9918WriteData
 * ----------------------------------------
 * write data (mode = 0) to the tms9918
 *
 * data: the data (DB0 -> DB7) to send
 */
VR_EMU_TMS9918_DLLEXPORT void __time_critical_func(vrEmuTms9918WriteData)(VrEmuTms9918* tms9918, uint8_t data)
{
  tms9918->regWriteStage = 0;
  tms9918->readAheadBuffer = data;
  tms9918->vram[(tms9918->currentAddress++) & VRAM_MASK] = data;
}


/* Function:  vrEmuTms9918ReadData
 * ----------------------------------------
 * read data (mode = 0) from the tms9918
 */
VR_EMU_TMS9918_DLLEXPORT uint8_t __time_critical_func(vrEmuTms9918ReadData)(VrEmuTms9918* tms9918)
{
  tms9918->regWriteStage = 0;
  uint8_t currentValue = tms9918->readAheadBuffer;
  tms9918->readAheadBuffer = tms9918->vram[(tms9918->currentAddress++) & VRAM_MASK];
  return currentValue;
}

/* Function:  vrEmuTms9918ReadDataNoInc
 * ----------------------------------------
 * read data (mode = 0) from the tms9918
 */
VR_EMU_TMS9918_DLLEXPORT uint8_t __time_critical_func(vrEmuTms9918ReadDataNoInc)(VrEmuTms9918* tms9918)
{
  return tms9918->readAheadBuffer;
}

/* Function:  vrEmuTms9918InterruptStatus
 * --------------------
 * return true if both INT status and INT control set
 */
VR_EMU_TMS9918_DLLEXPORT
bool __time_critical_func(vrEmuTms9918InterruptStatus)(VrEmuTms9918* tms9918)
{
  return tms9918->interruptsEnabled && (tms9918->status & STATUS_INT);
}


/* Function:  vrEmuTms9918OutputSprites
 * ----------------------------------------
 * Output Sprites to a scanline
 */
static void __time_critical_func(vrEmuTms9918OutputSprites)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const bool spriteMag = tmsSpriteMag(tms9918);
  const uint8_t spriteSize = tmsSpriteSize(tms9918);
  const bool sprite16 = spriteSize == 16;
  const uint8_t spriteSizePx = spriteSize * (spriteMag + 1);
  const uint16_t spriteAttrTableAddr = tmsSpriteAttrTableAddr(tms9918);
  const uint16_t spritePatternAddr = tmsSpritePatternTableAddr(tms9918);

  uint8_t spritesShown = 0;

  uint8_t* spriteAttr = tms9918->vram + spriteAttrTableAddr;
  for (uint8_t spriteIdx = 0; spriteIdx < MAX_SPRITES; ++spriteIdx)
  {
    int16_t yPos = spriteAttr[SPRITE_ATTR_Y];

    /* stop processing when yPos == LAST_SPRITE_YPOS */
    if (yPos == LAST_SPRITE_YPOS)
    {
      if ((tms9918->status & STATUS_5S) == 0)
      {
        tms9918->status |= spriteIdx;
      }
      break;
    }

    /* check if sprite position is in the -31 to 0 range and move back to top */
    if (yPos > 0xe0)
    {
      yPos -= 256;
    }

    /* first row is YPOS -1 (0xff). 2nd row is YPOS 0 */
    yPos += 1;

    int16_t pattRow = y - yPos;
    if (spriteMag)
    {
      pattRow >>= 1;  // this needs to be a shift because -1 / 2 becomes 0. Bad.
    }

    /* check if sprite is visible on this line */
    if (pattRow < 0 || pattRow >= spriteSize)
    {
      spriteAttr += SPRITE_ATTR_BYTES;
      continue;
    }

    if (spritesShown == 0)
    {
      int* rsbInt = (int*)tms9918->rowSpriteBits;
      int* end = rsbInt + sizeof(tms9918->rowSpriteBits) / sizeof(int);

      while (rsbInt < end)
      {
        *rsbInt++ = 0;
      }
    }

    /* have we exceeded the scanline sprite limit? */
    if (++spritesShown > MAX_SCANLINE_SPRITES)
    {
      if ((tms9918->status & STATUS_5S) == 0)
      {
        tms9918->status |= STATUS_5S | spriteIdx;
      }
      break;
    }

    /* sprite is visible on this line */
    const uint8_t spriteColor = spriteAttr[SPRITE_ATTR_COLOR] & 0x0f;
    const uint8_t pattIdx = spriteAttr[SPRITE_ATTR_NAME];
    const uint16_t pattOffset = spritePatternAddr + pattIdx * PATTERN_BYTES + (uint16_t)pattRow;

    const int16_t earlyClockOffset = (spriteAttr[SPRITE_ATTR_COLOR] & 0x80) ? -32 : 0;
    const int16_t xPos = (int16_t)(spriteAttr[SPRITE_ATTR_X]) + earlyClockOffset;

    int8_t pattByte = tms9918->vram[pattOffset];
    uint8_t screenBit = 0, pattBit = GRAPHICS_CHAR_WIDTH;

    int16_t endXPos = xPos + spriteSizePx;
    if (endXPos > TMS9918_PIXELS_X)
    {
      endXPos = TMS9918_PIXELS_X;
    }

    for (int16_t screenX = xPos; screenX < endXPos; ++screenX, ++screenBit)
    {
      if (screenX >= 0)
      {
        if (pattByte < 0)
        {
          if (spriteColor != TMS_TRANSPARENT && tms9918->rowSpriteBits[screenX] < 2)
          {
            pixels[screenX] = spriteColor;
          }
          /* we still process transparent sprites, since
             they're used in 5S and collision checks */
          if (tms9918->rowSpriteBits[screenX])
          {
            tms9918->status |= STATUS_COL;
          }
          else
          {
            tms9918->rowSpriteBits[screenX] = spriteColor + 1;
          }
        }
      }

      /* next pattern bit if non-magnified or if odd screen bit */
      if (!spriteMag || (screenBit & 0x01))
      {
        pattByte <<= 1;
        if (!--pattBit && sprite16) /* from A -> C or B -> D of large sprite */
        {
          pattBit = GRAPHICS_CHAR_WIDTH;
          pattByte = tms9918->vram[pattOffset + PATTERN_BYTES * 2];
        }
      }
    }
    spriteAttr += SPRITE_ATTR_BYTES;
  }

}


/* Function:  vrEmuTms9918GraphicsIScanLine
 * ----------------------------------------
 * generate a Graphics I mode scanline
 */
static void __time_critical_func(vrEmuTms9918GraphicsIScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const uint8_t tileY = y >> 3;   /* which name table row (0 - 23) */
  const uint8_t pattRow = y & 0x07;  /* which pattern row (0 - 7) */

  /* address in name table at the start of this row */
  const uint16_t rowNamesAddr = tmsNameTableAddr(tms9918) + tileY * GRAPHICS_NUM_COLS;

  const uint8_t* patternTable = tms9918->vram + tmsPatternTableAddr(tms9918) + pattRow;
  const uint8_t* colorTable = tms9918->vram + tmsColorTableAddr(tms9918);

  uint32_t* pixPtr = (uint32_t*)pixels;

  /* iterate over each tile in this row */
  for (uint8_t tileX = 0; tileX < GRAPHICS_NUM_COLS; ++tileX)
  {
    const uint8_t pattIdx = tms9918->vram[rowNamesAddr + tileX];
    uint8_t pattByte = patternTable[pattIdx * PATTERN_BYTES];
    const uint8_t colorByte = colorTable[pattIdx / GFXI_COLOR_GROUP_SIZE];

    int palette = ((tmsFgColor(tms9918, colorByte) << 4) | tmsBgColor(tms9918, colorByte)) * 16;

    *pixPtr++ = tms9918->pattLookup[palette + (pattByte >> 4)];
    *pixPtr++ = tms9918->pattLookup[palette + (pattByte & 0x0f)];
  }

  vrEmuTms9918OutputSprites(tms9918, y, pixels);
}

/* Function:  vrEmuTms9918GraphicsIIScanLine
 * ----------------------------------------
 * generate a Graphics II mode scanline
 */
static void __time_critical_func(vrEmuTms9918GraphicsIIScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const uint8_t tileY = y >> 3;   /* which name table row (0 - 23) */
  const uint8_t pattRow = y & 0x07;  /* which pattern row (0 - 7) */

  /* address in name table at the start of this row */
  const uint16_t rowNamesAddr = tmsNameTableAddr(tms9918) + tileY * GRAPHICS_NUM_COLS;

  /* the datasheet says the lower bits of the color and pattern tables must
     be all 1's for graphics II mode. however, the lowest 2 bits of the
     pattern address are used to determine if pages 2 & 3 come from page 0
     or not. Similarly, the lowest 6 bits of the color table register are
     used as an and mask with the nametable  index */
  const uint8_t nameMask = ((tms9918->registers[TMS_REG_COLOR_TABLE] & 0x7f) << 3) | 0x07;

  const uint16_t pageThird = ((tileY & 0x18) >> 3)
    & (tms9918->registers[TMS_REG_PATTERN_TABLE] & 0x03); /* which page? 0-2 */
  const uint16_t pageOffset = pageThird << 11; /* offset (0, 0x800 or 0x1000) */

  const uint8_t* patternTable = tms9918->vram + tmsPatternTableAddr(tms9918) + pageOffset + pattRow;
  const uint8_t* colorTable = tms9918->vram + tmsColorTableAddr(tms9918) + (pageOffset
    & ((tms9918->registers[TMS_REG_COLOR_TABLE] & 0x60) << 6)) + pattRow;

  uint32_t* pixPtr = (uint32_t*)pixels;

  /* iterate over each tile in this row */
  for (uint8_t tileX = 0; tileX < GRAPHICS_NUM_COLS; ++tileX)
  {
    uint8_t pattIdx = tms9918->vram[rowNamesAddr + tileX] & nameMask;

    const size_t pattRowOffset = pattIdx * PATTERN_BYTES;
    const uint8_t pattByte = patternTable[pattRowOffset];
    const uint8_t colorByte = colorTable[pattRowOffset];

    uint32_t* palPtr = tms9918->pattLookup + ((tmsFgColor(tms9918, colorByte) << 4) | tmsBgColor(tms9918, colorByte)) * 16;

    *pixPtr++ = palPtr[pattByte >> 4];
    *pixPtr++ = palPtr[pattByte & 0x0f];
  }

  vrEmuTms9918OutputSprites(tms9918, y, pixels);
}

/* Function:  vrEmuTms9918TextScanLine
 * ----------------------------------------
 * generate a Text mode scanline
 */
static void __time_critical_func(vrEmuTms9918TextScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const uint8_t tileY = y >> 3;   /* which name table row (0 - 23) */
  const uint8_t pattRow = y & 0x07;  /* which pattern row (0 - 7) */

  /* address in name table at the start of this row */
  const uint16_t rowNamesAddr = tmsNameTableAddr(tms9918) + tileY * TEXT_NUM_COLS;
  const uint8_t* patternTable = tms9918->vram + tmsPatternTableAddr(tms9918);

  uint32_t* palPtr = tms9918->pattLookup + tms9918->registers[TMS_REG_FG_BG_COLOR] * 16;

  /* fill the first 8 pixels with bg color */
  memset(pixels, tms9918->registers[TMS_REG_FG_BG_COLOR] & 0x0f, TEXT_PADDING_PX);
  uint32_t* pixPtr = (uint32_t*)(pixels + TEXT_PADDING_PX);

  for (uint8_t tileX = 0; tileX < TEXT_NUM_COLS; ++tileX)
  {
    const uint8_t pattIdx = tms9918->vram[rowNamesAddr + tileX];
    const uint8_t pattByte = patternTable[pattIdx * PATTERN_BYTES + pattRow];

    *pixPtr++ = palPtr[pattByte >> 4];
    *pixPtr = palPtr[pattByte & 0x0f];
    pixPtr = (uint32_t*)(((uint16_t*)pixPtr) + 1);
  }

  /* fill the last 8 pixels with bg color */
  memset(pixPtr, tms9918->registers[TMS_REG_FG_BG_COLOR] & 0x0f, TEXT_PADDING_PX);
}

/* Function:  vrEmuTms9918MulticolorScanLine
 * ----------------------------------------
 * generate a Multicolor mode scanline
 */
static void __time_critical_func(vrEmuTms9918MulticolorScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const uint8_t tileY = y >> 3;
  const uint8_t pattRow = ((y >> 2) & 0x01) + (tileY & 0x03) * 2;

  const uint8_t* nameTable = tms9918->vram + tmsNameTableAddr(tms9918) + tileY * GRAPHICS_NUM_COLS;
  const uint8_t* patternTable = tms9918->vram + tmsPatternTableAddr(tms9918) + pattRow;

  uint32_t* pixPtr = (uint32_t*)pixels;

  for (uint8_t tileX = 0; tileX < GRAPHICS_NUM_COLS; ++tileX)
  {
    const uint8_t colorByte = patternTable[nameTable[tileX] * PATTERN_BYTES];

    uint32_t* palPtr = tms9918->pattLookup + ((tmsFgColor(tms9918, colorByte) << 4) | tmsBgColor(tms9918, colorByte)) * 16;

    *pixPtr++ = palPtr[0xf];
    *pixPtr++ = palPtr[0x0];
  }

  vrEmuTms9918OutputSprites(tms9918, y, pixels);
}

/* Function:  vrEmuTms9918Text80ScanLine
 * ----------------------------------------
 * generate a Text mode scanline
 */
static void __time_critical_func(vrEmuTms9918Text80ScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  const uint8_t tileY = y >> 3;   /* which name table row (0 - 23) */
  const uint8_t pattRow = y & 0x07;  /* which pattern row (0 - 7) */

  /* address in name table at the start of this row */

  // Register 0x0A for text80 name table

  const uint16_t rowNamesAddr = /*tmsNameTableAddr(tms9918) +*/ tileY * TEXT80_NUM_COLS;
  const uint8_t* patternTable = tms9918->vram + tmsPatternTableAddr(tms9918);

  const vrEmuTms9918Color bgColor = TMS_DK_BLUE;//tmsMainBgColor(tms9918);
  const vrEmuTms9918Color fgColor = TMS_WHITE;//tmsMainFgColor(tms9918);

  uint8_t leftBgColor = (bgColor << 4);
  uint8_t leftFgColor = (fgColor << 4);

  /* fill the first and last 16 pixels with bg color */
  memset(pixels, leftBgColor | bgColor, TEXT_PADDING_PX);
  memset(pixels + TMS9918_PIXELS_X - TEXT_PADDING_PX, leftBgColor | bgColor, TEXT_PADDING_PX);

  for (uint8_t tileX = 0; tileX < TEXT80_NUM_COLS; ++tileX)
  {
    uint8_t pattIdx = tms9918->vram[rowNamesAddr + tileX];
    uint8_t pattByte = patternTable[pattIdx * PATTERN_BYTES + pattRow];
    for (uint8_t pattBit = 0; pattBit < TEXT_CHAR_WIDTH / 2; ++pattBit)
    {

      bool pixelBit = (pattByte << (pattBit * 2)) & 0x80;
      uint8_t doublePix = (uint8_t)(pixelBit ? leftFgColor : leftBgColor);
      pixelBit = (pattByte << (pattBit * 2 + 1)) & 0x80;
      doublePix |= (uint8_t)(pixelBit ? fgColor : bgColor);
      pixels[TEXT_PADDING_PX + tileX * (TEXT_CHAR_WIDTH / 2) + pattBit] = doublePix;
    }
  }
}


/* Function:  vrEmuTms9918ScanLine
 * ----------------------------------------
 * generate a scanline
 */
VR_EMU_TMS9918_DLLEXPORT void __time_critical_func(vrEmuTms9918ScanLine)(VrEmuTms9918* tms9918, uint8_t y, uint8_t pixels[TMS9918_PIXELS_X])
{
  if (!vrEmuTms9918DisplayEnabled(tms9918) || y >= TMS9918_PIXELS_Y)
  {
    memset(pixels, tmsMainBgColor(tms9918), TMS9918_PIXELS_X);
    if (y == TMS9918_PIXELS_Y - 1)
    {
      tms9918->status |= STATUS_INT;
    }
    return;
  }

  if (!tms9918->pattLookupInit)
  {
    tms9918->pattLookupInit = true;

    uint32_t* pattPtr = tms9918->pattLookup;
    for (int pal = 0; pal < 256; ++pal)
    {
      uint8_t fg = pal >> 4;
      uint8_t bg = pal & 0x0f;
      for (int patt = 0; patt < 16; ++patt)
      {
        uint32_t patternColors = 0;
        patternColors |= ((patt & 0x01) ? fg : bg) << 24;
        patternColors |= ((patt & 0x02) ? fg : bg) << 16;
        patternColors |= ((patt & 0x04) ? fg : bg) << 8;
        patternColors |= ((patt & 0x08) ? fg : bg);
        *pattPtr++ = patternColors;
      }
    }
  }

  switch (tms9918->mode)
  {
    case TMS_MODE_GRAPHICS_I:
      vrEmuTms9918GraphicsIScanLine(tms9918, y, pixels);
      break;

    case TMS_MODE_GRAPHICS_II:
      vrEmuTms9918GraphicsIIScanLine(tms9918, y, pixels);
      break;

    case TMS_MODE_TEXT:
      vrEmuTms9918TextScanLine(tms9918, y, pixels);
      break;

    case TMS_MODE_MULTICOLOR:
      vrEmuTms9918MulticolorScanLine(tms9918, y, pixels);
      break;

    case TMS_R0_MODE_TEXT_80:
      vrEmuTms9918Text80ScanLine(tms9918, y, pixels);
      break;
  }

  if (y == TMS9918_PIXELS_Y - 1)
  {
    tms9918->status |= STATUS_INT;
  }
}

/* Function:  vrEmuTms9918RegValue
 * ----------------------------------------
 * return a reigister value
 */
VR_EMU_TMS9918_DLLEXPORT
uint8_t __time_critical_func(vrEmuTms9918RegValue)(VrEmuTms9918* tms9918, vrEmuTms9918Register reg)
{
  return tms9918->registers[reg & 0x07];
}

/* Function:  vrEmuTms9918WriteRegValue
 * ----------------------------------------
 * write a reigister value
 */
VR_EMU_TMS9918_DLLEXPORT
void vrEmuTms9918WriteRegValue(VrEmuTms9918* tms9918, vrEmuTms9918Register reg, uint8_t value)
{
  int regIndex = reg & 0x07;
  tms9918->registers[regIndex] = value;
  if (regIndex < 2)
  {
    tms9918->mode = tmsMode(tms9918);
  }
}



/* Function:  vrEmuTms9918VramValue
 * ----------------------------------------
 * return a value from vram
 */
VR_EMU_TMS9918_DLLEXPORT
uint8_t vrEmuTms9918VramValue(VrEmuTms9918* tms9918, uint16_t addr)
{
  return tms9918->vram[addr & VRAM_MASK];
}

/* Function:  vrEmuTms9918DisplayEnabled
  * ----------------------------------------
  * check BLANK flag
  */
VR_EMU_TMS9918_DLLEXPORT
bool vrEmuTms9918DisplayEnabled(VrEmuTms9918* tms9918)
{
  return tms9918->registers[TMS_REG_1] & TMS_R1_DISP_ACTIVE;
}

/* Function:  vrEmuTms9918DisplayMode
  * --------------------
  * current display mode
  */
VR_EMU_TMS9918_DLLEXPORT
vrEmuTms9918Mode vrEmuTms9918DisplayMode(VrEmuTms9918* tms9918)
{
  return tms9918->mode;
}
