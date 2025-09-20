`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// stream_register_slice (2-entry skid buffer)
// - Breaks combinational ready path; prevents data loss on backpressure
// -----------------------------------------------------------------------------
module stream_register_slice #(
    parameter integer DATA_WIDTH    = 32,
    parameter integer STARTUP_DELAY = 0
)(
    input  wire                   clk,
    input  wire                   rst_n,
    // S-AXIS (from child)
    input  wire [DATA_WIDTH-1:0]  s_axis_tdata,
    input  wire                   s_axis_tvalid,
    output wire                   s_axis_tready,
    // M-AXIS (to system)
    output reg  [DATA_WIDTH-1:0]  m_axis_tdata,
    output reg                    m_axis_tvalid,
    input  wire                   m_axis_tready
);
    localparam integer SCNT_W = (STARTUP_DELAY <= 1) ? 1 :
                                (STARTUP_DELAY <= 2) ? 2 :
                                (STARTUP_DELAY <= 4) ? 3 :
                                (STARTUP_DELAY <= 8) ? 4 : 8;
    reg [SCNT_W-1:0] start_cnt;
    wire startup_done = (STARTUP_DELAY == 0) ? 1'b1
                        : (start_cnt == STARTUP_DELAY[SCNT_W-1:0]);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) start_cnt <= {SCNT_W{1'b0}};
        else if (!startup_done) start_cnt <= start_cnt + {{(SCNT_W-1){1'b0}},1'b1};
    end

    reg                  vld0, vld1;
    reg [DATA_WIDTH-1:0] dat0, dat1;

    assign s_axis_tready = !vld1;
    wire   push = s_axis_tvalid && s_axis_tready;
    wire   pop  = m_axis_tvalid && m_axis_tready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            vld0 <= 1'b0; vld1 <= 1'b0;
            dat0 <= {DATA_WIDTH{1'b0}};
            dat1 <= {DATA_WIDTH{1'b0}};
        end else begin
            if (pop) begin
                if (vld1) begin
                    dat0 <= dat1; vld0 <= 1'b1; vld1 <= 1'b0;
                end else begin
                    vld0 <= 1'b0;
                end
            end
            if (push) begin
                if (!vld0 && (!startup_done || (startup_done && !m_axis_tvalid))) begin
                    dat0 <= s_axis_tdata; vld0 <= 1'b1;
                end else begin
                    dat1 <= s_axis_tdata; vld1 <= 1'b1;
                end
            end
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {DATA_WIDTH{1'b0}};
        end else begin
            if (vld0) m_axis_tdata <= dat0;
            if (!startup_done) m_axis_tvalid <= 1'b0;
            else               m_axis_tvalid <= vld0 ? 1'b1 : 1'b0;
        end
    end
endmodule