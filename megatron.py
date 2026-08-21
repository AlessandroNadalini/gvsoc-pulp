from pulp.chips.megatron.pulp_open_board import Pulp_open_board
import gvsoc.runner

class Target(gvsoc.runner.Target):

    gapy_description = "Megatron virtual board"
    model = Pulp_open_board
    name = "megatron"

    def __init__(self, parser, options=None, name=None):
        super(Target, self).__init__(parser, options,
            model=Pulp_open_board, name=name)