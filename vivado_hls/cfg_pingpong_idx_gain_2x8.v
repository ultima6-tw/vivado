`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// cfg_pingpong_idx_gain_2x8
// - Double-buffer config for A/B × 8 tones: {index, gain}
// - Always write to shadow bank; commit atomically switches active bank
// -----------------------------------------------------------------------------
module cfg_pingpong_idx_gain_2x8 #(
    parameter integer IDX_W  = 10,
    parameter integer GAIN_W = 18
)(
    input  wire                clk,
    input  wire                rst_n,
    input  wire                idx_we,       // pulse: write index
    input  wire                gain_we,      // pulse: write gain
    input  wire                wr_ch,        // 0:A, 1:B
    input  wire [2:0]          wr_tone,      // 0..7
    input  wire [IDX_W-1:0]    wr_index,
    input  wire [GAIN_W-1:0]   wr_gain,
    input  wire                commit_req,
    input  wire                commit_safe,
    output reg                 active_bank,

    output wire [8*IDX_W -1:0] index_a_bus,
    output wire [8*GAIN_W-1:0] gain_a_bus,
    output wire [8*IDX_W -1:0] index_b_bus,
    output wire [8*GAIN_W-1:0] gain_b_bus
);
    reg [IDX_W -1:0] idx_a_0 [0:7], idx_a_1 [0:7];
    reg [GAIN_W-1:0] gn_a_0  [0:7], gn_a_1  [0:7];
    reg [IDX_W -1:0] idx_b_0 [0:7], idx_b_1 [0:7];
    reg [GAIN_W-1:0] gn_b_0  [0:7], gn_b_1  [0:7];

    wire shadow_bank = ~active_bank;

    localparam [GAIN_W-1:0] GAIN_ONE = (GAIN_W >= 18) ? 18'h1_0000
                                : {{(GAIN_W-1){1'b0}},1'b1};

    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active_bank <= 1'b0;
            for (i=0; i<8; i=i+1) begin
                idx_a_0[i] <= {IDX_W{1'b0}}; gn_a_0[i] <= {GAIN_W{1'b0}};
                idx_b_0[i] <= {IDX_W{1'b0}}; gn_b_0[i] <= {GAIN_W{1'b0}};
                idx_a_1[i] <= {IDX_W{1'b0}}; gn_a_1[i] <= {GAIN_W{1'b0}};
                idx_b_1[i] <= {IDX_W{1'b0}}; gn_b_1[i] <= {GAIN_W{1'b0}};
            end
        end else begin
            if (idx_we && wr_ch == 1'b0) begin
                if (shadow_bank == 1'b0) begin
                    idx_a_0[wr_tone] <= wr_index;
                end else begin
                    idx_a_1[wr_tone] <= wr_index;
                end
            end
            if (idx_we && wr_ch == 1'b1) begin
                if (shadow_bank == 1'b0) begin
                    idx_b_0[wr_tone] <= wr_index;
                end else begin
                    idx_b_1[wr_tone] <= wr_index;
                end
            end
            if (gain_we && wr_ch == 1'b0) begin
                if (shadow_bank == 1'b0) begin
                    gn_a_0[wr_tone] <= wr_gain;
                end else begin
                    gn_a_1[wr_tone] <= wr_gain;
                end
            end
            if (gain_we && wr_ch == 1'b1) begin
                if (shadow_bank == 1'b0) begin
                    gn_b_0[wr_tone] <= wr_gain;
                end else begin
                    gn_b_1[wr_tone] <= wr_gain;
                end
            end
            // Commit request + safe to commit
            if (commit_req && commit_safe) begin
                active_bank <= ~active_bank;
            end
        end
    end

    genvar t;
    generate
        for (t=0; t<8; t=t+1) begin : PACK
            assign index_a_bus[(t+1)*IDX_W -1 -: IDX_W] = (active_bank==1'b0) ? idx_a_0[t] : idx_a_1[t];
            assign gain_a_bus [(t+1)*GAIN_W-1 -: GAIN_W]= (active_bank==1'b0) ? gn_a_0 [t] : gn_a_1 [t];
            assign index_b_bus[(t+1)*IDX_W -1 -: IDX_W] = (active_bank==1'b0) ? idx_b_0[t] : idx_b_1[t];
            assign gain_b_bus [(t+1)*GAIN_W-1 -: GAIN_W]= (active_bank==1'b0) ? gn_b_0 [t] : gn_b_1 [t];
        end
    endgenerate
endmodule