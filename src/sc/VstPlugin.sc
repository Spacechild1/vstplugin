VstPlugin : UGen {
    *ar { |left, right|
        ^this.multiNew('audio', left, right);
    }
    checkInputs {
        [0, 1].do { |i|
            (inputs[i].rate != 'audio').if {
                ^"input % is not audio rate".format(i).throw;
            };
        };
        ^this.checkValidInputs;
    }
}
