VstPlugin : MultiOutUGen {
	*ar { arg inArray, nout=2;
		^this.multiNewList([\audio, nout] ++ inArray)
	}
	init { arg nout ... theInputs;
		inputs = theInputs;
		channels = Array.fill(nout, { arg i; OutputProxy(rate, this, i)});
		^channels
	}
	checkInputs {
		inputs.do({ arg item, i;
            (item.rate != \audio).if {
                ^"input % is not audio rate".format(i).throw;
            };
		});
        ^this.checkValidInputs;
    }
}
