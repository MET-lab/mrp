/*
 *  wavetables.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/18/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <cmath>
#include "wavetables.h"
#include "config.h"
using namespace std;

// Initialize all the tables we use
// Keep this in sync with the enum in the header file

WaveTable::WaveTable()
{
	float *table;
	int i, length;
	
	// Table 0: sine, size 4096
	
	length = 4096;
	table = new float[length + 1];	// +1 for guard point and interpolation
	
    for(i = 0; i < length; i++ )
    {
        table[i] = (float)sin(((double)i/(double)length) * M_PI * 2.);
    }
    table[length] = table[0]; /* set guard point */	
	
	tables_.push_back(table);
	lengths_.push_back(length);
	
	// Table 1 (TEST): linear, size 16
	
	length = 16;
	table = new float[length + 1];
	
	for(i = 0; i < length; i++ )
	{
		table[i] = (float)i / 16.;
	}
	table[length] = table[0];
	
	tables_.push_back(table);
	lengths_.push_back(length);	
}

// Return the table value that corresponds to index, which takes a range of 0-1.
// No interpolation, faster performance; allow wraparounds

float WaveTable::lookup(int table, float phase)
{
	int length = lengths_[table];
	float* buf = tables_[table];
	int index;
	
	if(phase >= 0)
		index = (int)((float)length * phase) % length;
	else
		index = (int)((float)length * phase) % length + length;
	
	return buf[index];
}

// Return the table value that corresponds to index, which takes a range of 0-1.
// With interpolation and wraparounds

float WaveTable::lookupInterp(int table, float phase)
{
	int length = lengths_[table];
	float* buf = tables_[table];
	float lo, hi, val;
	
	// First, calculate the fractional part, before applying wraparound
    float fIndex = phase * (float)length;
    int   index = (int)fIndex;
    float fract = fIndex - index;
	
	// Now handle the wraparound
	
	if(index >= 0)				// phase >= 0, 0 <= fract < 1
	{
		index = index % length;
		lo = buf[index];
		hi = buf[index + 1];
		val = lo + fract*(hi-lo);
	}
	else						// phase < 0, -1 < fract <= 0
	{
		index = index % length + length;
		lo = buf[index];
		hi = buf[index - 1];
		val = lo - fract*(hi-lo);
	}
	
    return val;
}

WaveTable::~WaveTable()
{
	// Free the allocated wavetables
	
	for(int i = 0; i < tables_.size(); i++)
		delete tables_[i];
}