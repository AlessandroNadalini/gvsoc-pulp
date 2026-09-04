#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <algorithm>
#include <stdio.h>

#include <pcm.hpp>

Pcm_HWPE::Pcm_HWPE(vp::ComponentConf &config) : vp::Component(config)
{
    // HWPE slave port
    this->hwpe_slv.set_req_meth(&Pcm_HWPE::hwpe_slave);
    this->new_slave_port("hwpe_slv", &this->hwpe_slv);
    
    // Streamer master port
    this->new_master_port("stream_mst", &this->stream_mst);

    // Done IRQ master port
    this->new_master_port("irq", &this->done);

    // Input & output streamers
    this->inp_stream = Pcm_HWPE_Streamer(this, false);
    this->out_stream = Pcm_HWPE_Streamer(this, true);

    // Engine
    this->engine = Pcm_HWPE_Engine(this);

    // Event handlers
    this->fsm_start_event = this->event_new(&Pcm_HWPE::fsm_start_handler);
    this->fsm_event = this->event_new(&Pcm_HWPE::fsm_handler);
    this->fsm_end_event = this->event_new(&Pcm_HWPE::fsm_end_handler);

    // Traces
    this->traces.new_trace("trace", &this->trace);
}

void Pcm_HWPE::reset(bool active) {
    if (active) {
        for (uint32_t i=0; i<56; i++)
            this->register_file[i] = 0x0;

        // Initial state of the controller FSM
        this->state.set(IDLE);

        // Done reset
        this->done.sync(false);

        // Job queue reset
        this->job_state = 0;
        this->job_pending = 0;
        this->cxt_cfg_ptr = 0;
        this->cxt_use_ptr = 0;
        this->job_id_counter = 0;
        this->running_job_id = -1;
        this->job = PcmJob();
        for (int i = 0; i < PCM_HWPE_N_CONTEXT; i++)
        {
            this->cxt_job_id[i] = -1;
            this->contexts[i] = PcmJob();
        }
    }
}

vp::IoReqStatus Pcm_HWPE::hwpe_slave(vp::Block *__this, vp::IoReq *req){
    Pcm_HWPE *_this = (Pcm_HWPE *)__this;
    uint32_t address = req->get_addr();

    if (req->get_is_write()) {
        uint32_t data = *((uint32_t *) (req->get_data()));

        _this->trace.msg(vp::TraceLevel::DEBUG, "Write request, address: 0x%x\n", address);

        if (address>PCM_HWPE_CHK_STATE)
        {
            if (address > 0xDC)
            {
                _this->trace.fatal("Trying to access invalid address 0x%x\n", address);

                return vp::IO_REQ_INVALID;
            }

            // Job-config registers always target the context currently open
            // for writing (cxt_cfg_ptr), never the flat register_file.
            uint32_t reg = PCM_JOB_REG_IDX(address);
            _this->trace.msg(vp::TraceLevel::DEBUG, "Writing job reg %d (ctx %d)\n", reg, _this->cxt_cfg_ptr);

            _this->contexts[_this->cxt_cfg_ptr].regs[reg] = data;
        } else {
            switch (address)
            {
            case PCM_HWPE_TRIG:
                _this->trace.msg(vp::TraceLevel::DEBUG, "Job triggered\n");
                _this->commit(true);
                break;
            case PCM_HWPE_SOFT_CLEAR:
                _this->trace.msg(vp::TraceLevel::DEBUG, "Soft clear\n");
                _this->soft_clear();
                break;
            default:
                // PCM_HWPE_ACQ / PCM_HWPE_FIN_JOBS / PCM_HWPE_STATUS /
                // PCM_HWPE_RUN_TASK / PCM_HWPE_CHK_STATE are read-only registers
                _this->trace.fatal("%x is a read-only register, cannot write %x\n", address, data);
                break;
            }
        }
    } else {
        _this->trace.msg(vp::TraceLevel::DEBUG, "Read request, address: 0x%x\n", address);

        uint32_t data;
        if (address > PCM_HWPE_CHK_STATE)
        {
            // Job-config registers: read back from the context currently
            // open for writing, mirroring the write redirection above.
            data = _this->contexts[_this->cxt_cfg_ptr].regs[PCM_JOB_REG_IDX(address)];
        }
        else
        {
            switch (address)
            {
            case PCM_HWPE_ACQ:
                data = (uint32_t) _this->acquire();
                break;
            case PCM_HWPE_FIN_JOBS:
                data = (_this->state.get() == IDLE && _this->job_pending == 0) ? 1 : 0;
                break;
            case PCM_HWPE_RUN_TASK:
                data = (uint32_t) _this->running_job_id;
                break;
            case PCM_HWPE_CHK_STATE:
                // Current controller FSM state (pcm_hwpe_state_t):
                // job-independent, it's simply the accelerator's own state.
                data = (uint32_t) _this->state.get();
                break;
            default:
                // PCM_HWPE_STATUS: unchanged flat passthrough
                data = _this->register_file[(address >> 2)];
                break;
            }
        }

        *(uint32_t *)req->get_data() = data;

        _this->trace.msg(vp::TraceLevel::DEBUG, "Read value: %x\n", *(uint32_t*)req->get_data());
    }

    return vp::IO_REQ_OK;
}

// ACQUIRE (read): assign a new job id and lock the write-side context.
int32_t Pcm_HWPE::acquire() {
    if (this->job_state != 0)
    {
        // Already acquired, not yet committed: re-return the pending id.
        return this->cxt_job_id[this->cxt_cfg_ptr];
    }
    else if (this->job_pending == PCM_HWPE_N_CONTEXT)
    {
        // Queue full
        return -1;
    }
    else
    {
        // Free to acquire and room in the queue: hand out a new id
        int32_t id = (int32_t) this->job_id_counter++;
        this->cxt_job_id[this->cxt_cfg_ptr] = id;
        this->job_state = -2;
        return id;
    }
}

// TRIGGER write: commit the context currently being configured
// and if the FSM is idle, start it.
void Pcm_HWPE::commit(bool start) {
    if (this->job_state == 0)
    {
        // Software wrote TRIGGER without ever reading ACQUIRE first (old,
        // single-job-style software): still queue the job.
        if (this->job_pending < PCM_HWPE_N_CONTEXT)
        {
            this->cxt_job_id[this->cxt_cfg_ptr] = (int32_t) this->job_id_counter;
            this->job_state = -2;
        }
    }
    if (this->job_state != -2)
    {
        this->trace.msg(vp::TraceLevel::WARNING,
            "TRIGGER dropped: job queue full (job_pending=%d)\n", this->job_pending);
        return;
    }

    this->job_pending++;
    this->job_state = 0;
    this->cxt_cfg_ptr = 1 - this->cxt_cfg_ptr;   // push: advance the write pointer

    if (start && this->state.get() == IDLE)
    {
        if (!this->fsm_start_event->is_enqueued())
        {
            this->event_enqueue(this->fsm_start_event, 1);
        }
    }
}

// SOFT_CLEAR write: full reset of the job queue and control FSM.
void Pcm_HWPE::soft_clear() {
    this->job_state = 0;
    this->job_pending = 0;
    for (int i = 0; i < PCM_HWPE_N_CONTEXT; i++)
    {
        this->cxt_job_id[i] = -1;
        this->contexts[i] = PcmJob();
    }
    this->running_job_id = -1;
    this->cxt_cfg_ptr = 0;
    this->cxt_use_ptr = 0;
    this->job_id_counter = 0;
    this->job = PcmJob();

    this->state.set(IDLE);
    this->register_file[PCM_HWPE_STATUS >> 2] = 0x0;
    this->done.sync(false);

    if (this->fsm_start_event->is_enqueued())
    {
        this->event_cancel(this->fsm_start_event);
    }
    if (this->fsm_event->is_enqueued())
    {
        this->event_cancel(this->fsm_event);
    }
    if (this->fsm_end_event->is_enqueued())
    {
        this->event_cancel(this->fsm_end_event);
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new Pcm_HWPE(config);
}

