/*
 *  filter.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/21/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <cstring>
#include "filter.h"
#include "config.h"

#pragma mark ButterBandpassFilter

// Create a new butterworth bandpass filter object, initializing 
ButterBandpassFilter::ButterBandpassFilter(float sampleRate)
{
	piDivSampleRate_ = M_PI / sampleRate;			// We use these derived parameters in the filter calculation
	twoPiDivSampleRate_ = 2.0 * piDivSampleRate_;
	
	bzero(a_, 5*sizeof(float));
	bzero(history_, 2*sizeof(float));
}

void ButterBandpassFilter::updateCoefficients(float frequency, float bandwidth)
{
	if(bandwidth <= 0.0 || frequency <= 0.0)	// Negative freq/bw makes no sense, just ignore it
		return;
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "BBP: freq = " << frequency << " bw = " << bandwidth << endl;
#endif
	
	float c, d;		// This code borrowed from csound Opcodes/butter.c
	
	c = 1.0 / tanf(piDivSampleRate_ * bandwidth);
	d = 2.0 * cosf(twoPiDivSampleRate_ * frequency);
	a_[0] = 1.0 / (1.0 + c);
	a_[1] = 0.0;
	a_[2] = -a_[0];
	a_[3] = - c * d * a_[0];
	a_[4] = (c - 1.0) * a_[0];
}

void ButterBandpassFilter::clearBuffer()
{
	bzero(history_, 2*sizeof(float));
}

#pragma mark GenericFilter

GenericFilter::GenericFilter(vector<double>& a, vector<double>& b)
{
	int i;
	
	aLength_ = a.size();
	bLength_ = b.size();
	
	if(aLength_ != 0)
	{
		a_ = new double[aLength_];
		aHistory_ = new double[aLength_];
		
		for(i = 0; i < a.size(); i++)
			a_[i] = a[i];
	}
	if(bLength_ != 0)
	{
		b_ = new double[bLength_];
		bHistory_ = new double[bLength_];

		for(i = 0; i < b.size(); i++)
			b_[i] = b[i];	
	}
	else	// Filter doesn't make sense without b coefficients-- make it a passthrough
	{
		bLength_ = 1;
		b_ = new double[1];
		bHistory_ = new double[1];
		b_[0] = 1.0;
	}
	
	clearBuffer();
}

GenericFilter::GenericFilter(const GenericFilter& copy)	// Copy constructor
{
	aLength_ = copy.aLength_;
	bLength_ = copy.bLength_;
	
	a_ = new double[aLength_];
	b_ = new double[bLength_];
	
	memcpy(a_, copy.a_, aLength_*sizeof(double));
	memcpy(b_, copy.b_, bLength_*sizeof(double));
	
	// Don't copy the current filter state
	aHistory_ = new double[aLength_];
	bHistory_ = new double[bLength_];
	
	clearBuffer();
}

GenericFilter& GenericFilter::operator=(const GenericFilter& copy)
{
	// First, free the current elements
	if(aLength_ != 0)
	{
		delete a_;
		delete aHistory_;
	}
	if(bLength_ != 0)
	{
		delete b_;
		delete bHistory_;
	}
	
	// Then, copy over the new data
	aLength_ = copy.aLength_;
	bLength_ = copy.bLength_;
	
	a_ = new double[aLength_];
	b_ = new double[bLength_];
	
	memcpy(a_, copy.a_, aLength_*sizeof(double));
	memcpy(b_, copy.b_, bLength_*sizeof(double));
	
	// Don't copy the current filter state
	aHistory_ = new double[aLength_];
	bHistory_ = new double[bLength_];
	
	clearBuffer();
	
	return *this;
}

// Delete the current set of coefficients and history and make a new one
void GenericFilter::updateCoefficients(vector<double>& a, vector<double>& b)
{
	int i;
	
	if(aLength_ != 0)
	{
		delete a_;
		delete aHistory_;
	}
	if(bLength_ != 0)
	{
		delete b_;
		delete bHistory_;
	}
	
	aLength_ = a.size();
	bLength_ = b.size();
	
	if(aLength_ != 0)
	{
		a_ = new double[aLength_];
		aHistory_ = new double[aLength_];
		
		for(i = 0; i < a.size(); i++)
			a_[i] = a[i];
	}
	if(bLength_ != 0)
	{
		b_ = new double[bLength_];
		bHistory_ = new double[bLength_];
		
		for(i = 0; i < b.size(); i++)
			b_[i] = b[i];	
	}
	else	// Filter doesn't make sense without b coefficients-- make it a passthrough
	{
		bLength_ = 1;
		b_ = new double[1];
		bHistory_ = new double[1];
		b_[0] = 1.0;
	}
	
	
	clearBuffer();
}

void GenericFilter::clearBuffer()		// Erase the current history
{
	bzero(aHistory_, aLength_*sizeof(double));
	bzero(bHistory_, bLength_*sizeof(double));
}

// Free all the memory we allocated
GenericFilter::~GenericFilter()
{
	if(aLength_ != 0)
	{
		delete a_;
		delete aHistory_;
	}
	if(bLength_ != 0)
	{
		delete b_;
		delete bHistory_;
	}
}

#pragma mark EnvelopeFollower

EnvelopeFollower::EnvelopeFollower(float timeConstant, float sampleRate)
{
	sampleRate_ = sampleRate;
	lastOutput_ = 0.0;
	
	// y = e^(-t/tau) where tau is the time constant
	// So the output falls to 1/e in the span of one time constant.  From this and the sample
	// rate we can figure out what each sample needs to be multiplied by.
	
	if(timeConstant == 0.0)
		decayScaler_ = 0.0;
	else
		decayScaler_ = exp(-1.0/(fabsf(timeConstant)*sampleRate_));
	
	if(decayScaler_ == 1.0)
		cerr << "Warning: envelope follower time constant rounded to 1.0\n";
}

void EnvelopeFollower::updateTimeConstant(float timeConstant)
{
	if(timeConstant == 0.0)
		decayScaler_ = 0.0;
	else
		decayScaler_ = exp(-1.0/(fabsf(timeConstant)*sampleRate_));
	
	if(decayScaler_ == 1.0)
		cerr << "Warning: envelope follower time constant rounded to 1.0\n";
}
