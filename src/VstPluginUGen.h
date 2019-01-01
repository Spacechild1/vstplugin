#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

class VstPluginUGen : public SCUnit {
public:
	VstPluginUGen();
	~VstPluginUGen();
	bool check();
	void printInfo();
	void open(const char *path);
	void close();
	void reset();
	void setParam(int32 index, float value);
	void mapParam(int32 index, int32 bus);
	
	void next(int inNumSamples);
private:
	struct Param {
		float value;
		int32 bus;
	};
	// helper methods
	const float *input(int i) const {
		return in(i + 3);
	}
	int numInChannels() const {
		return numInChannels_;
	}
	int numOutChannels() const {
		return numOutputs();
	}
	float readControlBus(int32 num);
	// data members
	IVSTPlugin *plugin_ = nullptr;
	float *buf_ = nullptr;
	const float **inBufVec_ = nullptr;
	float **outBufVec_ = nullptr;
	Param *paramVec_ = nullptr;
	int numInChannels_ = 0;
	bool gui_ = false;
	bool bypass_ = false;
};

template<typename T>
T clip(T in, T lo, T hi) {
	return std::max<T>(lo, std::min<T>(hi, in));
}
