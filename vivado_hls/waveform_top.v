`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// waveform_generator_v5 (Top Shell) -- Dual channel (A+B) with AXI-Stream cmd in
// - S_AXIS 32-bit commands -> decoder -> ping-pong config (A/B, 8 tones each)
// - SAFE command writes commit_safe_reg; COMMIT toggles active bank if safe==1
// - Two compute_core_child instances (A / B) produce Q1.31 streams
// - AND-join both channels and pack {A[31:16], B[31:16]} to 32-bit M_AXIS
// - Output register slice keeps timing clean
// -----------------------------------------------------------------------------
module waveform_generator_v5 #(
    parameter integer DATA_WIDTH    = 32,  // child stream data width (Q1.31 typical)
    parameter integer IDX_W         = 10,  // index width into phase/LUT tables
    parameter integer GAIN_W        = 18,  // gain width (e.g., Q1.17)
    parameter integer STARTUP_DELAY = 0
)(
    input  wire                   clk,
    input  wire                   rst_n,

    // S_AXIS command input (from PS or upstream IP)
    input  wire [31:0]            s_axis_tdata,
    input  wire                   s_axis_tvalid,
    output wire                   s_axis_tready,

    // Debug/monitor
    output wire                   active_bank_o,

    // Packed M_AXIS to system: {B[31:16], A[31:16]}
    output wire [31:0]            m_axis_tdata,
    output wire                   m_axis_tvalid,
    input  wire                   m_axis_tready
);
    // -------------------------------------------------------------------------
    // 1) AXIS command decoder (INDEX/GAIN/SAFE/COMMIT)
    // -------------------------------------------------------------------------
    wire               idx_we;
    wire               gain_we;
    wire               wr_ch;
    wire [2:0]         wr_tone;
    wire [IDX_W-1:0]   wr_index;
    wire [GAIN_W-1:0]  wr_gain;
    wire               commit_req;
    wire               safe_we;
    wire               safe_val;

    gpio_cfg_decoder_axis32 #(
        .IDX_W (IDX_W),
        .GAIN_W(GAIN_W)
    ) u_dec (
        .clk          (clk),
        .rst_n        (rst_n),
        .s_axis_tdata (s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),   // currently always 1
        .idx_we       (idx_we),
        .gain_we      (gain_we),
        .wr_ch        (wr_ch),
        .wr_tone      (wr_tone),
        .wr_index     (wr_index),
        .wr_gain      (wr_gain),
        .commit_req   (commit_req),
        .safe_we      (safe_we),
        .safe_val     (safe_val)
    );

    // Gate for committing; controlled by SAFE command
    reg commit_safe_reg;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            commit_safe_reg <= 1'b1;   // allow commit by default
        else if (safe_we)
            commit_safe_reg <= safe_val;
    end

    // -------------------------------------------------------------------------
    // 2) Ping-pong configuration banks (A/B, 8 tones each)
    // -------------------------------------------------------------------------
    wire [8*IDX_W -1:0] index_a_bus_int;
    wire [8*GAIN_W-1:0] gain_a_bus_int;
    wire [8*IDX_W -1:0] index_b_bus_int;
    wire [8*GAIN_W-1:0] gain_b_bus_int;
    wire                 active_bank_int;

    cfg_pingpong_idx_gain_2x8 #(
        .IDX_W (IDX_W),
        .GAIN_W(GAIN_W)
    ) u_cfg (
        .clk         (clk),
        .rst_n       (rst_n),
        .idx_we      (idx_we),
        .gain_we     (gain_we),
        .wr_ch       (wr_ch),
        .wr_tone     (wr_tone),
        .wr_index    (wr_index),
        .wr_gain     (wr_gain),
        .commit_req  (commit_req),
        .commit_safe (commit_safe_reg),
        .active_bank (active_bank_int),
        .index_a_bus (index_a_bus_int),
        .gain_a_bus  (gain_a_bus_int),
        .index_b_bus (index_b_bus_int),
        .gain_b_bus  (gain_b_bus_int)
    );

    assign active_bank_o = active_bank_int;

    // -------------------------------------------------------------------------
    // 3) Two child compute cores (Channel A / Channel B)
    // -------------------------------------------------------------------------
    // Channel A
    wire [DATA_WIDTH-1:0] coreA_tdata;
    wire                  coreA_tvalid;
    wire                  coreA_tready;

    compute_core_child #(
        .DATA_WIDTH(DATA_WIDTH),
        .IDX_W     (IDX_W),
        .GAIN_W    (GAIN_W)
    ) u_core_A (
        .clk          (clk),
        .rst_n        (rst_n),
        .index_a_bus  (index_a_bus_int),
        .gain_a_bus   (gain_a_bus_int),
        .m_axis_tdata (coreA_tdata),
        .m_axis_tvalid(coreA_tvalid),
        .m_axis_tready(coreA_tready)
    );

    // Channel B (reuse same child; feed B buses into the "A" ports)
    wire [DATA_WIDTH-1:0] coreB_tdata;
    wire                  coreB_tvalid;
    wire                  coreB_tready;

    compute_core_child #(
        .DATA_WIDTH(DATA_WIDTH),
        .IDX_W     (IDX_W),
        .GAIN_W    (GAIN_W)
    ) u_core_B (
        .clk          (clk),
        .rst_n        (rst_n),
        .index_a_bus  (index_b_bus_int),   // feed B
        .gain_a_bus   (gain_b_bus_int),
        .m_axis_tdata (coreB_tdata),
        .m_axis_tvalid(coreB_tvalid),
        .m_axis_tready(coreB_tready)
    );

    // -------------------------------------------------------------------------
    // 4) AXIS 2:1 AND-join and pack to 32-bit
    // -------------------------------------------------------------------------
    wire                 join_s_tready;
    wire                 join_s_tvalid = coreA_tvalid & coreB_tvalid;

    assign coreA_tready = join_s_tready & coreB_tvalid;
    assign coreB_tready = join_s_tready & coreA_tvalid;

    // Pack Q1.31 halves (drop 16 LSBs each)
    wire [31:0] join_s_tdata = { coreA_tdata[31:16], coreB_tdata[31:16] };

    // -------------------------------------------------------------------------
    // 5) AXIS register slice at output
    // -------------------------------------------------------------------------
    stream_register_slice #(
        .DATA_WIDTH   (32),
        .STARTUP_DELAY(STARTUP_DELAY)
    ) u_slice (
        .clk          (clk),
        .rst_n        (rst_n),
        .s_axis_tdata (join_s_tdata),
        .s_axis_tvalid(join_s_tvalid),
        .s_axis_tready(join_s_tready),
        .m_axis_tdata (m_axis_tdata),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready)
    );

endmodule