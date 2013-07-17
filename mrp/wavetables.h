/*
 *  wavetables.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/18/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef WAVETABLES_H
#define WAVETABLES_H

#include <iostream>
#include <vector>
using namespace std;

// List of presently defined tables; make sure the WaveTable constructor stays in sync with this
enum
{
	waveTableSine = 0
};

// Lookup tables for various waveforms
class WaveTable
{
public:
	WaveTable();
	// Return a non-interpolated value from a table
	float lookup(int table, float phase);
	// Return an interpolated value from a table
	float lookupInterp(int table, float phase);
	~WaveTable();
	
private:
	vector<float*> tables_;
	vector<int>	lengths_;
};

#endif // WAVETABLES_H