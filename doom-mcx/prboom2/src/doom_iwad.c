/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *  Copyright 2024 NXP
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  DOOM main program (D_DoomMain) and game loop (D_DoomLoop),
 *  plus functions to determine game mode (shareware, registered),
 *  parse command line parameters, configure game parameters (turbo),
 *  and call the startup functions.
 *
 *-----------------------------------------------------------------------------
 */

#pragma GCC optimize ("-O0")
#include "doom_iwad.h"
/*
const unsigned char doom_iwad[9UL] = {
0x49,0x57,0x41,0x44,0x86,0x04,0x00,0x00,0x0c};*/

//Uncomment which edition you want to compile
// UNO Q (STM32U585, 2MB internal flash, no external flash): the full 3.84MB
// DOOM1.WAD does not fit. Use DOOM Squashware (fragglet/squashware), ~1.62MB,
// which leaves ~200KB flash headroom over the ~228KB engine.
#include "iwad/squashware.c"
//#include "iwad/doom1.c"
//#include "iwad/doomu.c"
//#include "iwad/doom2.c"
//#include "iwad/tnt.c"
//#include "iwad/plutonia.c"
//#include "iwad/sigil.c"


const unsigned int doom_iwad_len = sizeof(doom_iwad);
