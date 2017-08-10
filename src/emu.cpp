#include "./emu.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include "SDL2/SDL.h"
#include "SDL2/SDL_endian.h"
#include "libzip/zip.h"

void draw();
// Debugging purposes
u_int32_t veces = 0;
u_int32_t cycles = 0;

// Refresh rate
u_int32_t refresh = (2000000 / 60) / 2;  // Interrupts
/* Interrupts: $cf (RST 0x08) at the start of vblank
 * $d7 (RST 0x10) at the end of vblank.*/
int INT = 0;  // Interrupts enabled flag

u_int8_t A;                 // Accumulator 8bit
u_int8_t B, C, D, E, H, L;  // General purpose registers 8bits
u_int16_t sp, pc;                // 16 bit stack pointer, program counter
u_int8_t memory[8192 * 8];  // 64 kilobytes of memory

// Ports
u_int8_t Read0 = 0x00;
u_int8_t Read1 = 0b10000011;
u_int8_t Read2 = 0b00000000;
u_int16_t ShiftRegister = 0x00;
u_int16_t noOfBitsToShift = 0x00;

u_int32_t shift0 = 0;
u_int32_t shift1 = 0;

// PSW
// F - Status register ... Not used in Space Invaders
unsigned char Z;   // Zero flag
unsigned char S;   // Sign flag
unsigned char P;   // Parity flag
unsigned char CY;  // Carry flag
unsigned char AC;  // Auxiliary carry flag ... Not used in Space Invaders

// Screen dimension constants
const int SCALE = 4;
// const int SCREEN_WIDTH = 224 * SCALE;
// const int SCREEN_HEIGHT = 256 * SCALE;

int decideINT = 0;

// The window we'll be rendering to
SDL_Window *gWindow = nullptr;

// The window renderer
SDL_Renderer *gRenderer = nullptr;

// The window surface
SDL_Surface *gScreenSurface = nullptr;
SDL_Surface *gScreenSurface2 = nullptr;
// Current displayed texture
SDL_Texture *gTexture = nullptr;

void loadRom(const char *file, int offset) {
  FILE *ROM = fopen(file, "rb");
  fseek(ROM, 0, SEEK_END);
  long size = ftell(ROM);
  rewind(ROM);

  // Allocate memory
  unsigned char *buffer = (unsigned char *)malloc(sizeof(unsigned char) * size);

  // Copy file to buffer
  fread(buffer, 1, size, ROM);
  for (int i = 0; i < size; i++) {
    memory[i + offset] = buffer[i];
  }
  fclose(ROM);
  free(buffer);
}

void loadRomZip(zip *z, const char *name, int offset) {
  struct zip_stat st;
  zip_stat_init(&st);
  zip_stat(z, name, 0, &st);

  zip_file *f = zip_fopen(z, name, 0);
  unsigned char *buffer = (unsigned char *)malloc(sizeof(char) * st.size);
  zip_fread(f, buffer, st.size);

  // Copy file to buffer
  for (int i = 0; i < st.size; i++) {
    memory[i + offset] = buffer[i];
  }
  free(buffer);

  zip_fclose(f);
}

int parity(int x, int size) {
  int i;
  int p = 0;
  x = (x & ((1 << size) - 1));
  for (i = 0; i < size; i++) {
    if (x & 0x1) p++;
    x = x >> 1;
  }
  return (0 == (p & 0x1));
}

void NOP() {
  pc += 1;
  cycles += 4;
}

void LXI(unsigned char const *opcode) {
  switch (*opcode) {
    case (0x01):
      C = opcode[1];
      B = opcode[2];
      break;
    case (0x11):
      E = opcode[1];
      D = opcode[2];
      break;
    case (0x21):
      L = opcode[1];
      H = opcode[2];
      break;
    case (0x31):
      sp = (opcode[2] << 8) | opcode[1];
      break;
  }
  pc += 3;
  cycles += 10;
}

void emulateCycle() {
  unsigned char *opcode = &memory[pc];
  unsigned int result;

  switch (*opcode) {
    case 0x00:
    case 0x10:
    case 0x20:
    case 0x30:
    case 0x08:
    case 0x18:
    case 0x28:
    case 0x38:
      NOP();
      break;

    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
      LXI(opcode);
      break;

    case (0x05):  // DCR B
      B--;
      // Zero flag
      if (B == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((B & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(B, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x06):  // MVI B, D8
      B = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0x09):  // DAD B
      // HL = HL + BC
      result = ((H << 8) | L) + ((B << 8) | C);
      H = result >> 8;
      L = result;
      // Carry flag
      if ((result) > 0xFFFF)
        CY = 1;
      else
        CY = 0;
      pc += 1;
      cycles += 10;
      break;

    case (0x0d):  // DCR C
      C--;
      // Zero flag
      if (C == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((C & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(C, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x0e):  // MVI C,D8
      C = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0x0f):  // RRC
      result = (A >> 1) | ((A & 0x1) << 7);
      CY = (A & 0x1);
      A = result & 0xff;
      pc += 1;
      cycles += 4;
      break;

    case (0x13):  // INX D
      // DE <- DE + 1
      result = ((D << 8) | E) + 1;
      E = result & 0xff;
      D = result >> 8;
      pc += 1;
      cycles += 5;
      break;

    case (0x19):  // DAD D
      // HL = HL + DE
      result = ((H << 8) | L) + ((D << 8) | E);
      H = result >> 8;
      L = result;
      // Carry flag
      if (result > 0xFFFF)
        CY = 1;
      else
        CY = 0;
      pc += 1;
      cycles += 10;
      break;

    case (0x1a):  // LDAX D
      // A <- (DE)
      A = memory[(D << 8) | E];
      pc += 1;
      cycles += 7;
      break;

    case (0x23):  // INX H
      // HL <- HL + 1
      result = ((H << 8) | L) + 1;
      L = result & 0x00FF;
      H = result >> 8;
      pc += 1;
      cycles += 5;
      break;

    case (0x26):  // MVI H,D8
      H = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0x29):  // DAD H
      // HL = HL + HL
      result = 2 * ((H << 8) | L);
      H = result >> 8;
      L = result;
      // Carry flag
      if (result > 0xFFFF)
        CY = 1;
      else
        CY = 0;
      pc += 1;
      cycles += 10;
      break;

    case (0x32):  // STA adr
      // (adr) <- A
      memory[opcode[1] | (opcode[2] << 8)] = A;
      pc += 3;
      cycles += 13;
      break;

    case (0x36):  // MVI M,D8
      // (HL) <- byte 2
      memory[(H << 8) | L] = opcode[1];
      pc += 2;
      cycles += 10;
      break;

    case (0x3a):  // LDA adr
      // A <- (adr)
      A = memory[opcode[1] | (opcode[2] << 8)];
      pc += 3;
      cycles += 13;
      break;

    case (0x3e):  // MVI A,D8
      // A <- byte 2
      A = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0x56):  // MOV D,M
      // D <- (HL)
      D = memory[(H << 8) | L];
      pc += 1;
      cycles += 7;
      break;

    case (0x5e):  // MOV E,M
      // E <- (HL)
      E = memory[(H << 8) | L];
      pc += 1;
      cycles += 7;
      break;

    case (0x66):  // MOV H,M
      // H <-(HL)
      H = memory[(H << 8) | L];
      pc += 1;
      cycles += 7;
      break;

    case (0x6f):  // MOV L,A
      L = A;
      pc += 1;
      cycles += 5;
      break;

    case (0x77):  // MOV M,A
      // (HL) <- A
      memory[(H << 8) | L] = A;
      pc += 1;
      cycles += 7;
      break;

    case (0x7a):  // MOV A,D
      A = D;
      pc += 1;
      cycles += 5;
      break;

    case (0x7b):  // MOV A,E
      A = E;
      pc += 1;
      cycles += 5;
      break;

    case (0x7c):  // MOV A,H
      A = H;
      pc += 1;
      cycles += 5;
      break;

    case (0x7e):  // MOV A,M
      A = memory[(H << 8) | L];
      pc += 1;
      cycles += 7;
      break;

    case (0xa7):  // ANA A
      // A <-A & A
      unsigned int a;
      a = A & A;
      A = a;
      // Zero flag
      if ((a & 0xFF) == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((a & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(a, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xaf):  // XRA A
      // A <-A ^ A
      A ^= A;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xc1):  // POP B
      // C <- (sp); B <- (sp+1); sp <- sp+2
      C = memory[sp];
      B = memory[sp + 1];
      sp += 2;
      pc += 1;
      cycles += 10;
      break;

    case (0xc2):  // JNZ adr
      // if NZ, PC <- adr
      if (Z == 0)
        pc = (opcode[1] | (opcode[2] << 8));
      else
        pc += 3;
      cycles += 10;
      break;

    case (0xc3):  // JMP adr
      pc = (opcode[1] | opcode[2] << 8);
      cycles += 10;
      break;

    case (0xc5):  // PUSH B
      // (sp-2)<-C; (sp-1)<-B; sp <- sp - 2
      memory[sp - 2] = C;
      memory[sp - 1] = B;
      sp = sp - 2;
      pc += 1;
      cycles += 11;
      break;

    case (0xc6):  // ADI D8
      // A <- A + byte
      // Carry flag
      if (A > (0xFF - opcode[1]))
        CY = 1;
      else
        CY = 0;
      A += opcode[1];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 2;
      cycles += 7;
      break;

    case (0xc9):  // RET
      // PC.lo <- (sp); PC.hi<-(sp+1); SP <- SP+2
      pc = (memory[(sp + 1)] << 8) | memory[sp];
      sp += 2;
      cycles += 10;
      break;

    case (0xcd):  // CALL
      // (SP-1)<-PC.hi;(SP-2)<-PC.lo;SP<-SP-2;PC=adr
      memory[sp - 1] = ((pc + 3) >> 8) & 0xff;
      memory[sp - 2] = ((pc + 3) & 0xff);
      sp -= 2;
      pc = (opcode[2] << 8) | opcode[1];
      cycles += 17;
      break;

    case (0xd1):  // POP D
      // E <- (sp); D <- (sp+1); sp <- sp+2
      E = memory[sp];
      D = memory[sp + 1];
      sp += 2;
      pc += 1;
      cycles += 10;
      break;

    case (0xd3):  // OUT D8
      // outputDevice[opcode[1]] = A;
      if (opcode[1] == 0x02) {
        noOfBitsToShift = A & 0x7;
      } else if (opcode[1] == 0x04) {
        shift0 = shift1;
        shift1 = A;
      }
      pc += 2;
      cycles += 10;
      break;

    case (0xd5):  // PUSH D
      memory[sp - 2] = E;
      memory[sp - 1] = D;
      sp -= 2;
      pc += 1;
      cycles += 11;
      break;

    case (0xe1):  // POP H
      L = memory[sp];
      H = memory[sp + 1];
      sp += 2;
      pc += 1;
      cycles += 10;
      break;

    case (0xe5):  // PUSH H
      // (sp-2)<-L; (sp-1)<-H; sp <- sp - 2
      memory[sp - 2] = L;
      memory[sp - 1] = H;
      sp -= 2;
      pc += 1;
      cycles += 11;
      break;

    case (0xe6):  // ANI D8
      // A <-A & data
      A &= opcode[1];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 2;
      cycles += 7;
      break;

    case (0xeb):  // XCHG
      // H <->D; L <->E
      unsigned char hold;
      hold = H;
      H = D;
      D = hold;
      hold = L;
      L = E;
      E = hold;
      pc += 1;
      cycles += 5;
      break;

    case (0xf1):  // POP PSW
      // flags <- (sp); A <- (sp+1); sp <- sp+2
      S = (memory[sp] >> 7) & 0x01;
      Z = (memory[sp] >> 6) & 0x01;
      // AC = (memory[sp] >> 4) & 0x01;
      P = (memory[sp] >> 2) & 0x01;
      CY = memory[sp] & 0x01;
      A = memory[sp + 1];
      sp += 2;
      pc += 1;
      cycles += 10;
      break;

    case (0xf5):  // PUSH PSW
      unsigned int psw;
      psw = 0x02;
      if (S == 1) psw |= 0x80;
      if (Z == 1) psw |= 0x40;
      // if (AC == 1)
      // psw |= 0x10;
      if (P == 1) psw |= 0x04;
      if (CY == 1) psw |= 0x01;
      memory[sp - 2] = psw;
      memory[sp - 1] = A;
      sp -= 2;
      pc += 1;
      cycles += 11;
      break;

    case (0xfb):  // EI
      INT = 1;
      pc += 1;
      cycles += 4;
      break;

    case (0xfe):  // CPI D8
      unsigned int x;
      x = A - opcode[1];

      if (A < opcode[1])
        CY = 1;
      else
        CY = 0;
      // if (A < opcode[1])
      // AC = 1;
      // else
      // AC = 0;
      if (x == 0)
        Z = 1;
      else
        Z = 0;
      if (0x80 == (x & 0x80))
        S = 1;
      else
        S = 0;
      P = parity(x, 8);
      pc += 2;
      cycles += 7;
      break;

    case (0x35):  // DCR M
      memory[(H << 8) | L] -= 1;
      // Zero flag
      if (memory[(H << 8) | L] == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((memory[(H << 8) | L] & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(memory[(H << 8) | L], 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 10;
      break;

    case (0xdb):  // IN para input
      if (opcode[1] == 0x01) {
        A = Read0;
      } else if (opcode[1] == 0x02) {
        A = Read1;
      } else if (opcode[1] == 0x03) {
        int dwval;
        dwval = (shift1 << 8) | shift0;
        A = dwval >> (8 - noOfBitsToShift);
      }
      pc += 2;
      cycles += 10;
      break;

    case (0xc8):  // RZ
      if (Z == 1) {
        pc = (memory[(sp + 1)] << 8) | memory[sp];
        cycles += 11;
        sp += 2;
      } else {
        cycles += 5;
        pc += 1;
      }
      break;

    case (0xda):  // JC
      if (CY == 1)
        pc = opcode[1] | (opcode[2] << 8);
      else
        pc += 3;
      cycles += 10;
      break;

    case (0xca):  // JZ
      if (Z == 1)
        pc = opcode[1] | (opcode[2] << 8);
      else
        pc += 3;
      cycles += 10;
      break;

    case (0x27): {  // DAA
      unsigned char ls = A & 0xf;
      if (ls > 9) {  // Or AC == 1
        A += 6;
      }

      unsigned char ms = (A & 0xf0) >> 4;
      if (ms > 9 || (CY == 1)) {
        ms += 6;
        if (ms > 0xf)
          CY = 1;
        else
          CY = 0;

        ms &= 0xf;
        A &= 0xf;
        A |= (ms << 4);
      }

      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);

      pc += 1;
      cycles += 4;
      break;
    }

    case (0x7d):  // MOV AL
      A = L;
      cycles += 5;
      pc += 1;
      break;

    case (0x3d):  // DCR A
      A--;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x80):  // ADD B
      // Carry flag
      if (A > (0xFF - B))
        CY = 1;
      else
        CY = 0;
      A += B;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x22):  // SHLD
      memory[(opcode[1] | (opcode[2] << 8))] = L;
      memory[(opcode[1] | (opcode[2] << 8)) + 1] = H;
      pc += 3;
      cycles += 16;
      break;

    case (0xd2):  // JNC
      if (CY == 0)
        pc = (opcode[1] | opcode[2] << 8);
      else
        pc += 3;
      cycles += 10;
      break;

    case (0x82):  // ADD D
      // Carry flag
      if (A > (0xFF - D))
        CY = 1;
      else
        CY = 0;
      A += D;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x17):  // RAL
      result = (A << 1) | CY;
      CY = ((A & 0x80) >> 7);
      A = result & 0xff;
      pc += 1;
      cycles += 4;
      break;

    case (0x4e):  // MOV CM
      C = memory[(H << 8) | L];
      cycles += 7;
      pc += 1;
      break;

    case (0x2a):  // LHLD
      L = memory[(opcode[1] | (opcode[2] << 8))];
      H = memory[(opcode[1] | (opcode[2] << 8)) + 1];
      cycles += 16;
      pc += 3;
      break;

    case (0x0a):  // LDAX B
      // A <- (BC)
      A = memory[(B << 8) | C];
      pc += 1;
      cycles += 7;
      break;

    case (0x37):  // STC
      CY = 1;
      pc += 1;
      cycles += 4;
      break;

    case (0x03):  // INX B
      // BC <- BC + 1
      result = ((B << 8) | C) + 1;
      B = result >> 8;
      C = result & 0xff;
      pc += 1;
      cycles += 5;
      break;

    case (0x67):  // MOV HA
      H = A;
      cycles += 5;
      pc += 1;
      break;

    case (0x5f):  // MOV EA
      E = A;
      cycles += 5;
      pc += 1;
      break;

    case (0x57):  // MOV DA
      D = A;
      cycles += 5;
      pc += 1;
      break;

    case (0xd8):  // RC
      if (CY == 1) {
        pc = ((memory[(sp + 1)] << 8) | (memory[sp]));
        sp += 2;
        cycles += 11;
      } else {
        pc += 1;
        cycles += 5;
      }
      break;

    case (0x4f):  // MOV CA
      C = A;
      cycles += 5;
      pc += 1;
      break;

    case (0x2e):  // MVI L,d8
      L = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0xb6):  // ORA M
      A |= memory[(H << 8) | L];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      CY = 0;  // Carry bit reset
      pc += 1;
      cycles += 7;
      break;

    case (0x46):  // MOV BM
      B = memory[(H << 8) | L];
      cycles += 7;
      pc += 1;
      break;

    case (0xb0):  // ORA B
      A |= B;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      CY = 0;  // Carry bit reset
      pc += 1;
      cycles += 4;
      break;

    case (0x79):  // MOV AC
      A = C;
      cycles += 5;
      pc += 1;
      break;

    case (0xe3):  // XTHL
      // L <-> (sp); H <-> (sp+1)
      unsigned char temp;
      temp = L;
      L = memory[sp];
      memory[sp] = temp;
      temp = H;
      H = memory[sp + 1];
      memory[sp + 1] = temp;
      pc += 1;
      cycles += 18;
      break;

    case (0xe9):  // PCHL
      pc = (H << 8) | L;
      cycles += 5;
      break;

    case (0xa8):  // XRA B
      // A <-A ^ B
      A ^= B;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xc0):  // RNZ
      if (Z == 0) {
        pc = (memory[(sp + 1)] << 8) | memory[sp];
        cycles += 11;
        sp += 2;
      } else {
        cycles += 5;
        pc += 1;
      }
      break;

    case (0xd0):  // RNC
      if (CY == 0) {
        pc = (memory[(sp + 1)] << 8) | memory[sp];
        cycles += 11;
        sp += 2;
      } else {
        cycles += 5;
        pc += 1;
      }
      break;

    case (0x2b):  // DCX H
      result = ((H << 8) | L) - 1;
      H = result >> 8;
      L = result & 0xFF;
      pc += 1;
      cycles += 5;
      break;

    case (0x78):  // MOV AB
      A = B;
      pc += 1;
      cycles += 5;
      break;

    case (0xd6):  // SUI d8
      // Carry flag
      if (A < opcode[1])
        CY = 1;
      else
        CY = 0;
      A -= opcode[1];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 2;
      cycles += 7;
      break;

    case (0x07):  // RLC
      result = (A << 1) | ((A & 0x08) >> 7);
      CY = ((A & 0x80) >> 7);
      A = result & 0xFF;
      pc += 1;
      cycles += 4;
      break;

    case (0x16):  // MVI D d8
      D = opcode[1];
      pc += 2;
      cycles += 7;
      break;

    case (0xc4):  // CNZ a16
      if (Z == 0) {
        memory[sp - 1] = ((pc + 3) >> 8) & 0xff;
        memory[sp - 2] = (pc + 3) & 0xff;
        sp -= 2;
        pc = (opcode[2] << 8) | opcode[1];
        cycles += 17;
      } else {
        pc += 3;
        cycles += 11;
      }
      break;

    case (0x1f):  // RAR
      result = (A >> 1) | (CY << 7);
      CY = (A & 0x1);
      A = result & 0xff;
      pc += 1;
      cycles += 4;
      break;

    case (0xf6):  // ORI d8
      // Carry flag
      if (A > (0xFF - opcode[1]))
        CY = 1;
      else
        CY = 0;
      A |= opcode[1];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 2;
      cycles += 7;
      break;

    case (0x04):  // INR B
      B++;
      // Zero flag
      if (B == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((B & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(B, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x70):  // MOV MB
      memory[(H << 8) | L] = B;
      pc += 1;
      cycles += 7;
      break;

    case (0xb4):  // ORA H
      A |= H;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      CY = 0;  // Carry bit reset
      pc += 1;
      cycles += 4;
      break;

    case (0x3c):  // INR A
      A++;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0xcc):  // CZ a16
      if (Z == 1) {
        memory[sp - 1] = ((pc + 3) >> 8) & 0xff;
        memory[sp - 2] = (pc + 3) & 0xff;
        sp = sp - 2;
        pc = (opcode[2] << 8) | opcode[1];
        cycles += 17;
      } else {
        pc += 3;
        cycles += 11;
      }
      break;

    case (0xfa):  // JM a16
      if (S == 1)
        pc = opcode[1] | (opcode[2] << 8);
      else
        pc += 3;
      cycles += 10;
      break;

    case (0x68):  // MOV LB
      L = B;
      pc += 1;
      cycles += 5;
      break;

    case (0x61):  // MOV HC
      H = C;
      pc += 1;
      cycles += 5;
      break;

    case (0xde):  // SBI d8
      result = opcode[1] + CY;
      // Carry
      if (A < result)
        CY = 1;
      else
        CY = 0;
      A -= result;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 2;
      cycles += 7;
      break;

    case (0x47):  // MOV BA
      B = A;
      pc += 1;
      cycles += 5;
      break;

    case (0x14):  // INR D
      D++;
      // Zero flag
      if (D == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((D & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(D, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x15):  // DCR D
      D--;
      // Zero flag
      if (D == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((D & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(D, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x86):  // ADD M
      // Carry flag
      if (A > (0xFF - memory[(H << 8) | L]))
        CY = 1;
      else
        CY = 0;
      A += memory[(H << 8) | L];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 7;
      break;

    case (0x69):  // MOV LC
      L = C;
      pc += 1;
      cycles += 5;
      break;

    case (0x34):  // INR M
      memory[(H << 8) | L]++;
      // Zero flag
      if (memory[(H << 8) | L] == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((memory[(H << 8) | L] & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(memory[(H << 8) | L], 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 10;
      break;

    case (0xb8):  // CMP B
      // Carry
      if (A < B)
        CY = 1;
      else
        CY = 0;
      result = A - B;
      // Zero flag
      if (result == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((result & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(result, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x85):  // ADD L
      // Carry flag
      if (A > (0xFF - L))
        CY = 1;
      else
        CY = 0;
      A += L;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xa0):  // ANA B
      // A <-A & B
      A &= B;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xbe):  // CMP M
      result = A - memory[(H << 8) | L];
      // Carry
      // if (A < memory[(H<<8)|L])
      if ((result & 0x100) != 0)
        CY = 1;
      else
        CY = 0;
      // Zero flag
      if (result == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((result & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(result, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x1b):  // DCX D
      result = ((D << 8) | E) - 1;
      D = result >> 8;
      E = result & 0xFF;
      pc += 1;
      cycles += 5;
      break;

    case (0x25):  // DCR H
      H--;
      // Zero flag
      if (H == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((H & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(H, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x12):  // STAX D
      memory[(D << 8) | E] = A;
      pc += 1;
      cycles += 7;
      break;

    case (0xbc):  // CMP H
      result = A - H;
      // Carry
      // if (A < B)
      if ((result & 0x100) != 0)
        CY = 1;
      else
        CY = 0;
      // Zero flag
      if (result == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((result & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(result, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0xd4):  // CNC a16
      if (CY == 0) {
        memory[sp - 1] = ((pc + 3) >> 8) & 0xff;
        memory[sp - 2] = (pc + 3) & 0xff;
        sp = sp - 2;
        pc = (opcode[2] << 8) | opcode[1];
        cycles += 17;
      } else {
        pc += 3;
        cycles += 11;
      }
      break;

    case (0xe8):  // RPE
      if (P != 0) {
        pc = (memory[(sp + 1)] << 8) | memory[sp];
        sp += 2;
        cycles += 11;
      } else {
        pc += 1;
        cycles += 5;
      }
      break;

    case (0xd9):  // RET
      // PC.lo <- (sp); PC.hi<-(sp+1); SP <- SP+2
      pc = (memory[(sp + 1)] << 8) | memory[sp];
      sp += 2;
      cycles += 10;
      break;

    case (0x9e):  // SBB M
      // A <- A - (HL) - Carry
      int res;
      res = A - memory[(H << 8) | L] - CY;
      if (res > 0xff)
        CY = 1;
      else
        CY = 0;
      A = res & 0xff;
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      CY = 1;  // Carry bit is reset to zero
      pc += 1;
      cycles += 4;
      break;

    case (0x40):  // MOV B,B
      B = B;
      pc += 1;
      cycles += 5;
      break;

    case (0x2c):  // INR L
      L++;
      // Zero flag
      if (L == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((L & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(L, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x2f):  // CM A
      A = ~A;
      cycles += 4;
      pc += 1;
      break;

    case (0xa6):  // ANA M
      // A <-A & (HL)
      A &= memory[(H << 8) | L];
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Carry flag
      CY = 0;  // Carry bit is reset to zero
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 7;
      break;

    case (0x71):  // MOV M C
      memory[(H << 8) | L] = C;
      pc += 1;
      cycles += 7;
      break;

    case (0x0c):  // INR C
      C++;
      // Zero flag
      if (C == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((C & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(C, 8);
      // Auxiliary flag - NOT IMPLEMENTED
      pc += 1;
      cycles += 5;
      break;

    case (0x65):  // MOV H L
      H = L;
      pc += 1;
      cycles += 5;
      break;

    case (0x41):  // MOV B C
      B = C;
      pc += 1;
      cycles += 5;
      break;

    case (0x81):  // ADD C
      // Carry flag
      if (A > (0xFF - C))
        CY = 1;
      else
        CY = 0;
      A += C;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x97):  // SUB A
      A -= A;
      Z = 1;
      CY = 0;
      // AC = 0;
      P = 1;
      S = 0;
      cycles += 4;
      pc += 1;
      break;

    case (0x48):  // MOV C, B
      C = B;
      cycles += 5;
      pc += 1;
      break;

    case (0x83):  // ADD E
      // Carry flag
      if (A > (0xFF - E))
        CY = 1;
      else
        CY = 0;
      A += E;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x8A):  // ADC D (A + D + CY)
      // Carry flag
      if (A > (0xFF - D))
        CY = 1;
      else
        CY = 0;
      A += D + CY;
      // Zero flag
      if (A == 0)
        Z = 1;
      else
        Z = 0;
      // Sign flag
      if ((A & 0x80) == 0x80)
        S = 1;
      else
        S = 0;
      // Parity flag
      P = parity(A, 8);
      // Auxiliary Carry - NOT IMPLEMENTED
      pc += 1;
      cycles += 4;
      break;

    case (0x73):  // MOV M,E
      // (HL) <- E
      memory[(H << 8) | L] = E;
      pc += 1;
      cycles += 7;
      break;

    case (0x72):  // MOV M,D
      // (HL) <- D
      memory[(H << 8) | L] = D;
      pc += 1;
      cycles += 7;
      break;

    default:
      printf("ERROR %x \n", *opcode);
      cycles += 4;
      break;
  }
}

void interruptExecute(int opcode) {
  switch (opcode) {
    case (0xcf):  // RST algo //11xxx111 001
      // short int ret = pc + 2;
      memory[sp - 1] = ((pc) >> 8) & 0xff;
      memory[sp - 2] = (pc)&0xff;
      sp -= 2;
      pc = 0x08;
      cycles += 11;
      break;

    case (0xd7):  // RST algo //010 11xxx111
      // short int ret = pc + 2;
      memory[sp - 1] = ((pc) >> 8) & 0xff;
      memory[sp - 2] = (pc)&0xff;
      sp -= 2;
      pc = 0x10;
      cycles += 11;
      break;
  }
}

bool init(void *window) {
  // Initialization flag
  bool success = true;

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL Error: %s\n", SDL_GetError());
    success = false;
  } else {
    // Set texture filtering to linear
    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1")) {
      printf("Warning: Linear texture filtering not enabled!");
    }

    // Create window
    // gWindow = SDL_CreateWindow("Space Invaders", SDL_WINDOWPOS_UNDEFINED,
    // SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    gWindow = SDL_CreateWindowFrom(window);
    if (gWindow == nullptr) {
      printf("Window could not be created! SDL Error: %s\n", SDL_GetError());
      success = false;
    } else {
      // Create renderer for window
      gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED);
      if (gRenderer == nullptr) {
        printf("Renderer could not be created! SDL Error: %s\n",
               SDL_GetError());
        success = false;
      } else {
        // Initialize renderer color
        SDL_SetRenderDrawColor(gRenderer, 0xFF, 0xFF, 0xFF, 0xFF);

        // Scaled window surface
        gScreenSurface2 =
            SDL_CreateRGBSurface(0, 224 * SCALE, 256 * SCALE, 32, 0, 0, 0, 0);
        // Game surface (original arcade size)
        gScreenSurface = SDL_CreateRGBSurface(0, 224, 256, 32, 0, 0, 0, 0);
        gTexture = SDL_CreateTextureFromSurface(gRenderer, gScreenSurface2);
      }
    }
  }

  return success;
}

void draw() {
  Uint32 pixel;
  Uint16 i;
  Uint32 *bits;
  Uint8 j;

  memset(gScreenSurface->pixels, 0, 256 * gScreenSurface->pitch);
  pixel = SDL_MapRGB(gScreenSurface->format, 0xFF, 0xFF, 0xFF);

  SDL_LockSurface(gScreenSurface);
  for (i = 0x2400; i < 0x3fff; i++) {
    if (memory[i] != 0) {
      for (j = 0; j < 8; j++) {
        if ((memory[i] & (1 << j)) != 0) {
          bits =
              (Uint32 *)gScreenSurface->pixels +
              ((255 - ((((i - 2400) % 0x20) << 3) + j)) * gScreenSurface->w) +
              ((i - 2400) >> 5) + 11;
          *bits = pixel;
        }
      }
    }
  }
  SDL_UnlockSurface(gScreenSurface);

  // Scale original surface to window surface
  SDL_BlitScaled(gScreenSurface, nullptr, gScreenSurface2, nullptr);

  // Update texture
  SDL_UpdateTexture(gTexture, nullptr, gScreenSurface2->pixels,
                    gScreenSurface2->pitch);

  // Clear screen
  SDL_RenderClear(gRenderer);

  // Render texture to screen
  SDL_RenderCopy(gRenderer, gTexture, nullptr, nullptr);

  // Update screen
  SDL_RenderPresent(gRenderer);
}

int main2(void *window, const char *zipFile) {
  // Load ROMs
  pc = 0x0;
  sp = 0xf000;
  bool repeat = true;

  std::string zipF(zipFile);

  zipF.erase(0, 6);  // Remove file://

  int err = 0;
  zip *z = zip_open(zipF.c_str(), 0, &err);

  loadRomZip(z, (const char *)"invaders.h", 0);
  loadRomZip(z, (const char *)"invaders.g", 0x800);
  loadRomZip(z, (const char *)"invaders.f", 0x1000);
  loadRomZip(z, (const char *)"invaders.e", 0x1800);

  zip_close(z);

  init(window);
  int decideINT = 0;

  while (repeat) {
    emulateCycle();
    veces++;

    if ((INT == 1) && (cycles >= refresh)) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
          repeat = false;
        } else if (event.type == SDL_KEYDOWN) {
          switch (event.key.keysym.sym) {
            case (SDLK_0): {
              Read0 |= 0b00000001;
              break;
            }
            case (SDLK_2): {
              Read0 |= 0b00000010;
              break;
            }
            case (SDLK_1): {
              Read0 |= 0b00000100;
              break;
            }
            case (SDLK_SPACE): {
              Read0 |= 0b00010000;
              Read1 |= 0b00010000;
              break;
            }
            case (SDLK_LEFT): {
              Read0 |= 0b00100000;
              Read1 |= 0b00100000;
              break;
            }
            case (SDLK_RIGHT): {
              Read0 |= 0b01000000;
              Read1 |= 0b01000000;
              break;
            }
          }
        } else if (event.type == SDL_KEYUP) {
          switch (event.key.keysym.sym) {
            case (SDLK_0): {
              Read0 &= 0b11111110;
              break;
            }
            case (SDLK_2): {
              Read0 &= 0b11111101;
              break;
            }
            case (SDLK_1): {
              Read0 &= 0b11111011;
              break;
            }
            case (SDLK_SPACE): {
              Read0 &= 0b11101111;
              Read1 &= 0b11101111;
              break;
            }
            case (SDLK_LEFT): {
              Read0 &= 0b11011111;
              Read1 &= 0b11011111;
              break;
            }
            case (SDLK_RIGHT): {
              Read0 &= 0b10111111;
              Read1 &= 0b10111111;
              break;
            }
          }
        }
      }

      decideINT == 0 ? interruptExecute(0xcf) : interruptExecute(0xd7);
      decideINT = decideINT == 0 ? 1 : 0;
      INT = 0;

      draw();
      SDL_Delay(10);
      cycles = 0;
    }
  }
  return 0;
}