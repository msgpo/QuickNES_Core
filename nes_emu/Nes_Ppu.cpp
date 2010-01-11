
// Timing and behavior of PPU

// Nes_Emu 0.5.6. http://www.slack.net/~ant/

#include "Nes_Ppu.h"

#include <string.h>
#include "Nes_Snapshot.h"

/* Copyright (C) 2004-2005 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
more details. You should have received a copy of the GNU Lesser General
Public License along with this module; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

// to do: implement junk in unused bits when reading registers

#include BLARGG_SOURCE_BEGIN

ppu_time_t const t_to_v_time = 20 * scanline_len + 302;
ppu_time_t const even_odd_time = 20 * scanline_len + 328;

int const scanline_render_col = 60;
ppu_time_t const first_scanline_time = 21 * scanline_len + scanline_render_col;
ppu_time_t const first_hblank_time = 21 * scanline_len + 251;

inline ppu_time_t Nes_Ppu::ppu_time( nes_time_t t ) const
{
	return t * ppu_overclock + extra_clocks;
}

Nes_Ppu::Nes_Ppu()
{
}

Nes_Ppu::~Nes_Ppu()
{
}

// Rendering behavior

// returns true when sprite hit checking is done for the frame (hit flag won't change any more)
inline bool Nes_Ppu::update_sprite_hit( nes_time_t cpu_time )
{
	ppu_time_t const earliest_sprite_hit = 21 * scanline_len + 339;
	int sprite_y = spr_ram [0];
	ppu_time_t time = cpu_time * ppu_overclock - earliest_sprite_hit;
	if ( sprite_y >= 239 )
		return true;
	
	ppu_time_t earliest = sprite_y * scanline_len + spr_ram [3];
	if ( time < earliest )
		return false;
	
	if ( false )
	{
		// debug version with fixed hit point
		ppu_time_t hit_time = earliest + 3 * scanline_len;
		if ( time < hit_time )
			return false;
		r2002 |= 0x40;
		return true;
	}
	
	int count_needed = 1 + time / scanline_len + 1;
	if ( count_needed > 240 )
		count_needed = 240;
	render_until( cpu_time ); // avoids rendering every scanline individually
	while ( scanline_count < count_needed )
		render_until( next_time );
	
	if ( !sprite_hit_y )
		return time >= earliest + scanline_len * 16;
	
	ppu_time_t hit_time = (sprite_hit_y - 1) * scanline_len + sprite_hit_x;
	if ( time < hit_time )
		return false;
	
	r2002 |= 0x40;
	return true;
}

void Nes_Ppu::run_scanlines( int const count )
{
	hblank_time += scanline_len * (count - 1);
	
	if ( !scanline_count )
		first_scanline();
	
	if ( w2001 & palette_changed )
	{
		palette_changed = 0;
		capture_palette();
	}
	
	int save_vaddr = vram_addr;
	
	int begin = scanline_count;
	scanline_count = begin + count;
	
	if ( scanline_count >= 240 && !host_palette_size() )
	{
		capture_palette();
		palette_changed = false;
	}
	
	if ( caller_pixels )
	{
		render_scanlines( begin, count,
				caller_pixels + caller_row_bytes * begin, caller_row_bytes );
	}
	else if ( (w2001 & 0x18) == 0x18 )
	{
		int y = spr_ram [0] + 1;
		int skip = min( count, max( y - begin, 0 ) );
		run_hblank( skip );
		
		int visible = min( count - skip, sprite_height() );
		//dprintf( "count: %d, sprite_y: %d, skip: %d, visible: %d\n",
		//      count, y, skip, visible );
		
		assert( skip + visible <= count );
		assert( visible <= mini_offscreen_height );
		
		if ( visible > 0 )
			render_scanlines( begin + skip, visible, impl->mini_offscreen, buffer_width );
	}
	
	vram_addr = save_vaddr; // to do: this is cheap
	run_hblank( count - 1 );
}

void Nes_Ppu::render_until_( nes_time_t cpu_time )
{
	ppu_time_t time = cpu_time * ppu_overclock;
	ppu_time_t const frame_duration = scanline_len * 261;
	if ( time > frame_duration )
		time = frame_duration;
	
	if ( frame_phase == 0 )
	{
		frame_phase = 1;
		if ( w2001 & 0x08 )
			vram_addr = vram_temp;
	}
	
	if ( frame_phase == 1 )
	{
		if ( time <= even_odd_time )
		{
			next_time = even_odd_time / ppu_overclock + 1;
			return;
		}
		
		frame_phase = 2;
		if ( !(w2001 & 0x08) || odd_frame )
		{
			extra_clocks--;
			if ( extra_clocks < 0 )
			{
				extra_clocks = 2;
				frame_length_++;
			}
		}
	}
	
	if ( hblank_time < scanline_time && hblank_time < time )
	{
		hblank_time += scanline_len;
		run_hblank( 1 );
	}
	
	int count = 0;
	while ( scanline_time < time )
	{
		scanline_time += scanline_len;
		count++;
	}
	
	if ( count )
		run_scanlines( count );
	
	if ( hblank_time < time )
	{
		hblank_time += scanline_len;
		run_hblank( 1 );
	}
	assert( time <= hblank_time );
	
	next_time = (scanline_time < hblank_time ? scanline_time : hblank_time) / 3 + 1;
}

// Frame timing

inline void Nes_Ppu::suspend_rendering()
{
	// prevent render_until() from causing any activity
	next_time = LONG_MAX / 2;
}

void Nes_Ppu::load_state( Nes_Snapshot const& in )
{
	// lightweight reset
	suspend_rendering();
	set_nt_banks( 0, 0, 0, 0 );
	set_chr_bank( 0, 0x2000, 0 );
	odd_frame = false;
	extra_clocks = 0;
	
	Nes_Ppu_Impl::load_state( in );
	nmi_will_occur = (w2000 & 0x80); // to do: incorrect
}

void Nes_Ppu::reset( bool full_reset )
{
	suspend_rendering();
	
	if ( full_reset )
	{
		Nes_Ppu_Impl::reset();
		memset( host_palette, 0, sizeof host_palette );
	}
	
	w2000 = 0;
	w2001 = 0;
	r2002 = 0x80; // VBL flag immediately set at power-up, but about 27400 clocks after reset
	w2003 = 0;
	r2007 = 0;
	second_write = false;
	vram_addr = 0;
	vram_temp = 0;
	pixel_x = 0;
	odd_frame = false;
	extra_clocks = 0;
	nmi_will_occur = false;
}

nes_time_t Nes_Ppu::begin_frame()
{
	int nmi_time = LONG_MAX / 2; // to do: common constant for "will never occur"
	if ( nmi_will_occur )
		nmi_time = (extra_clocks == 2 ? 1 : 2);
	
	nmi_will_occur = false;
	ppu_time_t frame_len = 341 * 262 - 1 - extra_clocks;
	frame_length_ = (frame_len + 2) / 3;
	extra_clocks = frame_length_ * 3 - frame_len;
	check( (unsigned) extra_clocks < 3 );
	
	//if ( w2000 & 0x80 && !!(r2002 & 0x80) != (nmi_time >= 0) )
	//  dprintf( "r2002: %d, nmi_time: %d\n", (int) r2002, (int) nmi_time );
	
	next_time = t_to_v_time / ppu_overclock;
	hblank_time = first_hblank_time;
	scanline_time = first_scanline_time;
	scanline_count = 0;
	frame_phase = 0;
	query_phase = 0;
	end_vbl_mask = ~0xe0;
	palette_changed = 0x18;
	
	//dprintf( "spr hit y: %d, x: %d\n", sprite_hit_y, sprite_hit_x );
	
	sprite_hit_y = 0;
	sprite_hit_x = 0;
	
	w2003 = 0;
	
	return nmi_time;
}

void Nes_Ppu::query_until( nes_time_t time )
{
	// nothing happens until scanline 20
	if ( time > (2272 + (extra_clocks >> 1)) )
	{
		// clear VBL, sprite hit, and max sprites flags first time after 20 scanlines
		r2002 &= end_vbl_mask;
		end_vbl_mask = ~0;
		
		// sprite hit
		if ( query_phase < 2 && update_sprite_hit( time ) )
			query_phase = 2;
		
		if ( time >= ((21 + 164) * scanline_len + 100) / ppu_overclock )
		{
			// to do: implement max sprites properly?
			r2002 |= 0x20;
			//if ( !(r2002 & 0x20) && time >= earliest_sprite_max / ppu_overclock )
			//  update_sprite_max( time );
			
			// update frame_length
			if ( time >= 29770 && query_phase < 3 )
			{
				render_until( time );
			
				// set VBL when end of frame is reached
				if ( time >= frame_length() )
				{
					r2002 |= 0x80;
					query_phase = 3;
					nmi_will_occur = (w2000 & 0x80);
				}
			}
		}
	}
}

inline int Nes_Ppu::read_status( nes_time_t time )
{
	query_until( time );
	second_write = false;
	
	// special vbl behavior when read is just before or at clock when it's set
	if ( extra_clocks != 2 )
	{
		if ( time == frame_length() )
			nmi_will_occur = false;
	}
	else if ( time == frame_length() - 1 )
	{
		r2002 &= ~0x80;
		query_phase = 3;
		nmi_will_occur = false;
	}
	
	int result = r2002;
	r2002 &= ~0x80;
	return result;
}

void Nes_Ppu::end_frame( nes_time_t end_time )
{
	render_until( end_time );
	query_until( end_time );
	odd_frame ^= 1;
	
	if ( w2001 & 0x08 )
	{
		// to do: do more PPU RE to get exact behavior
		unsigned a = vram_addr;
		if ( (a & 0xff) < 0xfe )
		{
			a += 2;
		}
		else
		{
			a ^= 0x400;
			a -= 0x1e;
		}
		vram_addr = a;
	}
}

// Register read/write

int Nes_Ppu::read( nes_time_t time, unsigned addr )
{
	if ( addr > 0x2007 )
		dprintf( "Read from mirrored PPU register 0x%04X\n", addr );
	
	// Don't catch rendering up to present since status reads don't affect
	// rendering and status is often polled in a tight loop.
	
	switch ( addr & 7 )
	{
		// status
		case 2:
			return read_status( time );
		
		// sprite ram
		case 4: {
			int result = spr_ram [w2003];
			if ( (w2003 & 3) == 2 )
				result &= 0xe3;
			return result;
		}
		
		// video ram
		case 7:
			render_until( time ); // changes to vram_addr affect rendering
			return read_2007( time );
	}
	
	return 0;
}

void Nes_Ppu::write_( nes_time_t time, unsigned addr, int data )
{
	if ( addr > 0x2007 )
		dprintf( "Wrote to mirrored PPU register 0x%04X\n", addr );
	
	int reg = (addr & 7);
	if ( reg == 0 )
	{
		// render only if changes to register might affect graphics
		if ( (((vram_temp >> 10) ^ data) & 3) | ((w2000 ^ data) & 0x38) )
			render_until( time );
		
		if ( time >= 29770 && (w2000 ^ data) & 0x80 )
		{
			render_until( time );
			query_until( time - 1 + (extra_clocks >> 1) );
			// to do: time - 1 might cause problems
		}
		
		vram_temp = (vram_temp & ~0x0c00) | ((data & 3) * 0x400);
		w2000 = data;
		return;
	}
	/*
	// to do: remove
	if ( w2001 & 0x08 )
	{
		ppu_time_t const first_scanline = 22 * scanline_len;
		ppu_time_t t = time * ppu_overclock - first_scanline;
		if ( t >= 0 )
		{
			int line = t / scanline_len;
			//dprintf( "%d.%d $%04X<-$%02X\n", line, int (t - line * scanline_len),
			//      addr, data );
		}
	}
	*/
	render_until( time );
	switch ( reg )
	{
		//case 0: // control (handled above)
		
		case 1: // sprites, bg enable
			w2001 = data;
			break;
		
		case 3: // spr addr
			w2003 = data;
			break;
		
		case 4:
			spr_ram [w2003] = data;
			w2003 = (w2003 + 1) & 0xff;
			break;
		
		case 5:
			if ( !(second_write ^= 1) )
			{
				vram_temp = (vram_temp & ~0x73e0) |
						((data & 0xf8) << 2) | ((data & 7) << 12);
			}
			else
			{
				pixel_x = data & 7;
				vram_temp = (vram_temp & ~0x001f) | (data >> 3);
			}
			break;
		
		case 6:
			if ( !(second_write ^= 1) )
			{
				int old = ~vram_addr & vaddr_clock_mask;
				vram_addr = vram_temp = (vram_temp & ~0x00ff) | data;
				if ( vram_temp & old )
					mapper->a12_clocked( time );
			}
			else
			{
				vram_temp = (vram_temp & ~0xff00) | (data << 8 & 0x3f00);
			}
			break;
		
		case 7: // handled inline
			if ( write_2007( time, data ) )
				palette_changed = 0x18;
			break;
	}
}

