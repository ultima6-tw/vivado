`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// gpio_cfg_decoder_axis32
// - AXI-Stream 32-bit command decoder (one command per beat)
// - Command word format:
//     [31:28] CMD   : 1=INDEX, 2=GAIN, C=SAFE, F=COMMIT
//     [27]    CH    : 0=A, 1=B
//     [26:24] TONE  : 0..7
//     [23:20] RSV   : 0
//     [19:0]  DATA  : payload (index[IDX_W-1:0] or gain[17:0] or SAFE bit0)
// - Pulsed outputs (1 cycle when a matching command is accepted).
// - Always-ready sink by default (s_axis_tready=1). If you need backpressure
//   later, add a small skid buffer and drive tready accordingly.
// -----------------------------------------------------------------------------
module gpio_cfg_decoder_axis32 #(
    parameter integer IDX_W  = 10,
    parameter integer GAIN_W = 18
)(
    input  wire         clk,
    input  wire         rst_n,

    // S_AXIS command input
    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,

    // Decoded pulses
    output reg          idx_we,
    output reg          gain_we,
    output reg          wr_ch,
    output reg  [2:0]   wr_tone,
    output reg  [IDX_W-1:0]  wr_index,
    output reg  [GAIN_W-1:0] wr_gain,
    output reg          commit_req,
    output reg          safe_we,
    output reg          safe_val
);
    // Fields
    wire [3:0]  cmd   = s_axis_tdata[31:28];
    wire        ch    = s_axis_tdata[27];
    wire [2:0]  tone  = s_axis_tdata[26:24];
    wire [19:0] data  = s_axis_tdata[19:0];

    localparam [3:0] CMD_IDX            = 4'h1;
    localparam [3:0] CMD_GAIN           = 4'h2;
    localparam [3:0] CMD_IDX_COMMIT     = 4'h3;
    localparam [3:0] CMD_GAIN_COMMIT    = 4'h4;
    localparam [3:0] CMD_SAFE           = 4'hC;
    localparam [3:0] CMD_COMMIT         = 4'hF;

    // Backpressure logic
    reg stall_one;
    assign s_axis_tready = ~stall_one;
    wire accept = s_axis_tvalid & s_axis_tready;

    // Delay register for commit
    reg commit_delay;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ch           <= 1'b0;
            wr_tone         <= 3'd0;
            idx_we          <= 1'b0;
            gain_we         <= 1'b0;
            wr_index        <= {IDX_W{1'b0}};
            wr_gain         <= {GAIN_W{1'b0}};
            commit_req      <= 1'b0;
            commit_delay    <= 1'b0;
            safe_we         <= 1'b0;
            safe_val        <= 1'b0;
            stall_one       <= 1'b0;
        end else begin
            // default deassert pulses
            commit_req      <= 1'b0;
            safe_we         <= 1'b0;
            idx_we          <= 1'b0;
            gain_we         <= 1'b0;
            stall_one       <= 1'b0;

            // delay commit flag
            if (commit_delay) begin
                commit_req   <= 1'b1;
                commit_delay <= 1'b0;
                stall_one    <= 1'b1; // prevent new command in commit cycle
            end

            if (accept) begin
                case (cmd)
                    CMD_IDX: begin
                        idx_we    <= 1'b1;
                        wr_ch    <= ch;
                        wr_tone  <= tone;
                        wr_index <= data[IDX_W-1:0];
                    end
                    CMD_GAIN: begin
                        gain_we     <= 1'b1;
                        wr_ch       <= ch;
                        wr_tone     <= tone;
                        wr_gain     <= data[GAIN_W-1:0]; // assume Q1.17 in bits[17:0]
                        stall_one   <= 1'b1; // bubble for commit
                    end
                    CMD_IDX_COMMIT: begin
                        idx_we      <= 1'b1;
                        wr_ch       <= ch;
                        wr_tone     <= tone;
                        wr_index    <= data[IDX_W-1:0];
                        commit_delay <= 1'b1; // next bit commit
                        stall_one   <= 1'b1; // bubble for commit
                    end
                    CMD_GAIN_COMMIT: begin
                        gain_we  <= 1'b1;
                        wr_ch    <= ch;
                        wr_tone  <= tone;
                        wr_gain  <= data[GAIN_W-1:0];
                        commit_delay <= 1'b1; // next bit commit
                    end
                    CMD_COMMIT: begin
                        commit_req <= 1'b1;
                        stall_one  <= 1'b1; // bubble for commit
                    end
                    CMD_SAFE: begin
                        safe_we  <= 1'b1;
                        safe_val <= data[0]; // 1=allow commit, 0=block
                    end
                    default: ; // no-op
                endcase
            end
        end
    end
endmodule