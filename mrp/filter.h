/*
 *  filter.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/21/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef FILTER_H
#define FILTER_H

#include <iostream>
#include <cmath>
#include <vector>
using namespace std;

// Second-order Butterworth bandpass filter, whose coefficients can be updated in real-time

class ButterBandpassFilter
{
public:
	ButterBandpassFilter(float sampleRate);
	
	void updateCoefficients(float frequency, float bandwidth);	// Update the filter coefficients
	float filter(float sample) {								// Process a new sample
		float t, y;												// Do this inline for time efficiency
		t = sample - a_[3] * history_[0] - a_[4] * history_[1];
		y = t * a_[0] + a_[1] * history_[0] + a_[2] * history_[1];
		history_[1] = history_[0];
		history_[0] = t;
		return y;
	}
	void clearBuffer();											// Zero out the history
	
private:
	float twoPiDivSampleRate_;	// 2.0*pi/sampleRate
	float piDivSampleRate_;		// pi/sampleRate
	
	float a_[5];
	float history_[2];		// Need two back-samples of memory to calculate 2nd-order filter
};

// Generic filter structure, coefficients can be changed but this clears all state information
// (i.e. this is not well-suited to ramps, etc).  We use this for the PLL loop filter

class GenericFilter
{
public:
	GenericFilter(vector<double>& a, vector<double>& b);		// Don't even need the sample rate here
	GenericFilter(const GenericFilter& copy);					// Copy constructor
	GenericFilter& operator=(const GenericFilter& copy);
	
	void updateCoefficients(vector<double>& a, vector<double>& b);
	double filter(double sample) {								// Process a new sample
		double y = b_[0]*sample;
		int i;
		
		bHistory_[0] = sample;
		for(i = bLength_-1; i > 0; i--)
		{
			y += b_[i]*bHistory_[i];
			bHistory_[i] = bHistory_[i-1];
		}
		if(aLength_ > 0)	// Only do this part if we have poles
		{
			for(i = aLength_-1; i > 0; i--)
			{
				y -= a_[i]*aHistory_[i];
				aHistory_[i] = aHistory_[i-1];
			}
			y -= a_[0]*aHistory_[0];
			aHistory_[0] = y;
		}
		
		return y;
	}
	void clearBuffer();											// Zero out the history
	
	~GenericFilter();
private:
	double *a_, *b_, *aHistory_, *bHistory_;
	unsigned int aLength_, bLength_;
};

// An envelope follower acts as an ideal full-wave rectifier followed by a parallel resistor and capacitor:
// Peaks in the signal are followed exactly, with a first-order decay between peaks.

class EnvelopeFollower
{
public:
	EnvelopeFollower(float timeConstant, float sampleRate);
	
	void updateTimeConstant(float timeConstant);
	float filter(float sample) {
		float ret;
		float absSample = fabsf(sample);
		
		if(absSample > lastOutput_)
			ret = absSample;
		else
			ret = lastOutput_*decayScaler_;
		
		return (lastOutput_ = ret);
	}
	float currentValue() { return lastOutput_; }	// Return current value without changing it
	void clearBuffer() { lastOutput_ = 0.0; }
	
private:
	float sampleRate_;
	float decayScaler_;
	float lastOutput_;
};

#endif // FILTER_H