/*
 *  parameter.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/19/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "parameter.h"
#include "config.h"

ostream& operator<<(ostream& output, const Parameter& p)
{
	output << "val_ = " << p.val_ << ", oldValue_ = " << p.oldValue_ << ", stepValue_ = " << p.stepValue_ << ", shape = ";
	switch(p.shape_)
	{
		case shapeHold:
			output << "hold\n";
			break;
		case shapeLinear:
			output << "linear\n";
			break;
		case shapeLogarithmic:
			output << "log\n";
			break;
		case shapeStep:
			output << "step\n";
			break;
		default:
			output << "unknown (" << p.shape_ << ")\n";
	}
	//output << "    sampleRate_ = " << p.sampleRate_ << ", updateRate_ = " << p.updateRate_ << endl;
	output << "    rampList_: ";
	if(p.rampList_.size() == 0)
		output << "(empty)";
	for(int i = 0; i < p.rampList_.size(); i++)
	{
		output << "[v " << p.rampList_[i].nextValue << ", d " << p.rampList_[i].duration << ", ";
		switch(p.rampList_[i].shape)
		{
			case shapeHold:
				output << "hold";
				break;
			case shapeLinear:
				output << "lin";
				break;
			case shapeLogarithmic:
				output << "log";
				break;
			case shapeStep:
				output << "st";
				break;
			default:
				output << "? (" << p.rampList_[i].shape << ")";
		}
		output << "] ";
	}
	output << endl;
	return output;
}

Parameter::Parameter(double initialValue, float sampleRate)
{			
	val_ = oldValue_ = initialValue;
	sampleRate_ = (double)sampleRate;
	updateRate_ = sampleRate_/(double)PARAMETER_UPDATE_INTERVAL; 
	stepValue_ = 0.0;
	shape_ = shapeHold;
}	

void Parameter::setCurrentValue(double newValue)
{
	val_ = newValue;
	rampList_.clear();			// Clear any future ramps
	
	startRamp();				// This will hold the current value
}

void Parameter::setRampValues(double startValue, timedParameter& rampValues)
{
	val_ = startValue;			// Set current value
	rampList_ = rampValues;		// Replace the ramp deque with the new values
	
	startRamp();
}

void Parameter::appendRampValues(timedParameter& rampValues)
{
	rampList_.insert(rampList_.end(), rampValues.begin(), rampValues.end());

#ifdef DEBUG_MESSAGES_EXTRA
	cout << "appendRampValues()\n";
#endif
	
	if(shape_ == shapeHold)	// If we're waiting, not in the middle of a current ramp, begin a new one
		startRamp();
}

bool Parameter::ramp(int numSamples)	// Returns true if the parameter has changed
{
	if(shape_ == shapeHold)		// Do nothing if we're holding the current value
		return false;
	
	double beginningValue = val_;
	
	sampleCount_++;
	if(sampleCount_ >= sampleMax_)
	{
		// Reached our target; set the new value to the target in case there was any drift
		if(rampList_.size() > 0)
		{
			val_ = rampList_[0].nextValue;
			// Pop the current parameterValue off the deque
			rampList_.pop_front();
		}
		// Set up the new ramp
		startRamp();
		
		return (val_ != beginningValue);
	}
	
	// Update the current value
	switch(shape_)
	{
		case shapeLinear:
			if(numSamples == PARAMETER_UPDATE_INTERVAL)
				val_ += stepValue_;
			else if(rampList_.size() > 0)	// Calculate the long (slow) way
			{
				val_ = oldValue_ + (rampList_[0].nextValue - oldValue_)*(double)sampleCount_/(double)(sampleMax_==0?1:sampleMax_);
			}
			// else can't do anything! (shouldn't happen)
			break;
		case shapeLogarithmic:
			if(numSamples == PARAMETER_UPDATE_INTERVAL)
				val_ *= stepValue_;
			else if(rampList_.size() > 0)	// Calculate the long (slow) way
			{	// No divide by 0; use 0.001 (-60dB) in this case
				if(rampList_[0].nextValue == 0.0)
					val_ = oldValue_ * pow(0.001/(oldValue_==0.0?0.001:oldValue_), (double)sampleCount_/(double)(sampleMax_==0?1:sampleMax_));
				else
					val_ = oldValue_ * pow(rampList_[0].nextValue/(oldValue_==0.0?0.001:oldValue_), (double)sampleCount_/(double)(sampleMax_==0?1:sampleMax_));
			}
			break;
		case shapeStep:
		case shapeHold:
		default:
			break;			// No change until we get to the next target
	}
	
	return (val_ != beginningValue);
}

// Start a ramp to the next value given at the beginning of the ramp list

void Parameter::startRamp()
{
	sampleCount_ = 0;
	oldValue_ = val_;
	
	if(rampList_.size() == 0)	// Hold this value indefinitely
	{
		stepValue_ = 0;
		shape_ = shapeHold;
		sampleMax_ = (unsigned int)0xFFFFFFFF;		// Biggest int value
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "holding at " << val_ << endl;
#endif		
	}
	else
	{
		shape_ = rampList_[0].shape;
		double nextValue = rampList_[0].nextValue;
		double duration = rampList_[0].duration;
		sampleMax_ = (int)(duration*updateRate_);
		
		// Calculate a step size based on the ramp shape
		switch(shape_)
		{
			case shapeLinear: // Linear (additive) steps
				stepValue_ = (nextValue-oldValue_)/(duration*updateRate_);
				break;
			case shapeLogarithmic: // Logarithmic (multiplicative) steps -- avoid dividing by 0!
				if(oldValue_ == 0.0) // Can't divide by 0 or ramp from it, so start at -60dB
					val_ = oldValue_ = 0.001;
				if(nextValue == 0.0)
					nextValue = 0.001;	// Similarly, ramp down to -60dB
				stepValue_ = pow(nextValue/oldValue_, 1.0/(duration*updateRate_));
				if(stepValue_ == 1.0 && nextValue != oldValue_)
					cerr << "Warning: Logarithmic step = 1.0\n";
				break;
			case shapeStep: // No step at all until the next value
			case shapeHold:
			default:
				stepValue_ = 0;
				break;
		}
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "oldValue = " << oldValue_ << ", nextValue = " << nextValue;
		cout << ", duration = " << duration << ", sampleRate = " << sampleRate_ << ", sampleMax_ = " << sampleMax_ << ", stepValue_ = " << stepValue_ << endl;
#endif
	}
}