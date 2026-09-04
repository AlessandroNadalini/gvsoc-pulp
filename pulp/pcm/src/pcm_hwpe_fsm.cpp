#include <pcm.hpp>

// Load the queued job in cxt_use_ptr: pop its snapshot into the active-job
// struct (this->job). Everything downstream (fsm_start_handler's streamer 
// configure() calls, the FSM, the engine) reads this->job instead of register_file
// for job-scoped registers so a concurrently-configured next job's writes can never
// reach it.
void Pcm_HWPE::start_next_job() {
    this->running_job_id = this->cxt_job_id[this->cxt_use_ptr];
    this->done.sync(false);   // deassert before the new job's completion pulse

    this->job = this->contexts[this->cxt_use_ptr];

    this->trace.msg(vp::TraceLevel::DEBUG, "Starting job %d (ctx %d)\n",
        this->running_job_id, this->cxt_use_ptr);
}

void Pcm_HWPE::fsm_start_handler(vp::Block *__this, vp::ClockEvent *event) {
    Pcm_HWPE* _this = (Pcm_HWPE *)__this;

    _this->start_next_job();

    // Configuration of the input streamer
    _this->trace.msg(vp::TraceLevel::DEBUG, "Configuring input stream...\n");
    _this->inp_stream.configure(
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_JOB_SRC_ADDR)],       // base_addr
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_TOTAL_LENGTH)],       // tot_len -- TO BE CHECKED
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_D0_LENGTH)],          // d0_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_D0_STRIDE)],          // d0_stride
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_D1_LENGTH)],          // d1_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_D1_STRIDE)],          // d1_stride
        0,                                                             // d2_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_D2_STRIDE)],          // d2_stride
        0                                                              // d3_stride
    );

    // Configuration of the output streamer
    _this->trace.msg(vp::TraceLevel::DEBUG, "Configuring output stream...\n");
    _this->out_stream.configure(
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_JOB_DST_ADDR)],       // base_addr
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_TOTAL_LENGTH)],   // tot_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_D0_LENGTH)],      // d0_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_D0_STRIDE)],      // d0_stride
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_D1_LENGTH)],      // d1_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_D1_STRIDE)],      // d1_stride
        0,                                                             // d2_len
        _this->job.regs[PCM_JOB_REG_IDX(PCM_HWPE_OUT_D2_STRIDE)],      // d2_stride
        0                                                              // d3_stride
    );

    _this->state.set(WRITE_RF); // TO BE CHECKED!!
    _this->fsm_loop();
}

void Pcm_HWPE::fsm_handler(vp::Block *__this, vp::ClockEvent *event) {
    Pcm_HWPE* _this = (Pcm_HWPE *)__this;

    _this->fsm_loop();
}

void Pcm_HWPE::fsm_end_handler(vp::Block *__this, vp::ClockEvent *event){
    Pcm_HWPE* _this = (Pcm_HWPE *) __this;

    // Retire the job that just finished (queue "pop"-completion)
    _this->cxt_job_id[_this->cxt_use_ptr] = -1;
    _this->cxt_use_ptr = 1 - _this->cxt_use_ptr;
    if (_this->job_pending > 0) _this->job_pending--;

    _this->state.set(IDLE);
    //_this->trace.msg(vp::TraceLevel::DEBUG, "Setting state to IDLE...\n");
    _this->register_file[PCM_HWPE_STATUS>>2] = 0x1;
    _this->done.sync(true);

    // Auto-chain: dispatch the next queued job without waiting for a new TRIG
    if (_this->job_pending > 0 && !_this->fsm_start_event->is_enqueued())
    {
        _this->event_enqueue(_this->fsm_start_event, 1);
    }
}

void Pcm_HWPE::fsm_loop() {
    uint32_t latency = 0;

    do
    {
        latency = this->fsm();
    } while (latency == 0 && state.get() != FINISHED); // FINISHED not defined, figure out what it corresponds to

    if (state.get() == FINISHED && !this->fsm_end_event->is_enqueued())  // FINISHED not defined, figure out what it corresponds to
    {
        this->event_enqueue(this->fsm_end_event, latency);
    } else if (!this->fsm_event->is_enqueued())
    {
        this->event_enqueue(this->fsm_event, latency);
    }
    this->trace.msg(vp::TraceLevel::DEBUG, "FSM ended\n");
}

int Pcm_HWPE::fsm() {
    auto next_state = this->state.get();

    int latency = 0;
    switch (this->state.get())
    {
    case WRITE_RF:
        next_state = CONFIG;
        break;

    case CONFIG:
        uint32_t ctrl_value;
        ctrl_value = 0x00FFFF01; // Just for testing
        this->trace.msg(vp::TraceLevel::DEBUG, "Configuring AIMC core with control value 0x%x\n", ctrl_value);

        latency += this->engine.handle_config(this, CMD_REGISTER, &ctrl_value, true);
        
        next_state = FINISHED;
        break;
    case FINISHED:
        this->trace.msg(vp::TraceLevel::DEBUG, "Finished MVM computation(s)\n");
        break;
    default:
        this->trace.fatal("PCM HWPE FSM: UNKNOWN STATE (%d)!\n", this->state.get());
    }

    this->state.set(next_state);
    return latency;
}