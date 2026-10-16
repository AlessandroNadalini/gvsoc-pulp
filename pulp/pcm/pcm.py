import gvsoc.systree

# Signed bit-width of the MVM accumulator full scale: int8 inputs x int4 weights
# accumulated over 512 rows give |acc| <= 128 * 8 * 512 = 2^19.
PCM_ACC_BITS = 20

class Pcm(gvsoc.systree.Component):

    def __init__(self,
                parent:gvsoc.systree.Component,
                name: str,
                mvm_latency: int=0,
                stim_file:str=None,
                out_bits: int=11):
        super().__init__(parent, name)

        # Outputs are streamed out as int16
        assert 2 <= out_bits <= 16, f"PCM out_bits must be in [2, 16], got {out_bits}"

        # Requantization maps the accumulator full scale onto out_bits (ADC-like):
        # y = clip(round(acc * requant_scale), -2^(out_bits-1), 2^(out_bits-1)-1)
        requant_scale = 2.0 ** -(PCM_ACC_BITS - out_bits)

        self.set_component('pulp.pcm.pcm')

        self.add_properties({
            "mvm_latency"   : mvm_latency,
            "stim_file"     : stim_file,
            "out_bits"      : out_bits,
            "requant_scale" : requant_scale
        })

    def i_hwpe_slv(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'hwpe_slv')

    def o_stream_mst(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('stream_mst', itf, signature='io')

    def set_stim_file(self, stim_file: str):
        """Update the stimulus file path for the PCM module."""
        self.add_properties({
            "stim_file": stim_file
        })
