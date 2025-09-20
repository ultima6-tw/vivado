`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// compute_core_child.v  (Channel-A only)
// - Pipeline (per-sample when downstream ready):
//     index/gain cache -> phase accum -> LUT -> 8x Q-mul (AXIS) -> adder_tree8 (AXIS)
// - Output: sum of 8 tones (Channel A), Q1.31 (DATA_WIDTH)
// - Handshake policy inside:
//     * Local "can_load" gates phase advance and downstream loads
//     * All 8 multipliers: s_axis_tvalid=advance, m_axis_tready=can_load
//     * Adder tree: s_axis_tvalid fires when all mul lanes valid AND can_load
// -----------------------------------------------------------------------------
module compute_core_child #(
    parameter integer DATA_WIDTH   = 32,   // Q1.31
    parameter integer IDX_W        = 10,   // index width to phase_incr table
    parameter integer GAIN_W       = 18,   // e.g., Q1.17
    parameter integer PHASE_W      = 32,   // phase accumulator width
    parameter integer LUT_ADDR_W   = 12,   // log2(LUT_DEPTH)
    parameter integer LUT_DEPTH    = (1<<LUT_ADDR_W),
    parameter integer FRAC_BITS    = 17,   // fractional bits of gain

    // Init files
    parameter         PHASE_INCR_INIT = "phase_incr_origin.mem", // small table in regs
    parameter         SINE_LUT_INIT   = "lut_q31_origin.mem"     // Q1.31 LUT
)(
    input  wire                        clk,
    input  wire                        rst_n,

    // Active configuration buses for Channel A (packed {tone7..tone0})
    input  wire [8*IDX_W -1:0]         index_a_bus,
    input  wire [8*GAIN_W-1:0]         gain_a_bus,

    // AXI-Stream out (sum of Channel A tones)
    output wire [DATA_WIDTH-1:0]       m_axis_tdata,
    output wire                        m_axis_tvalid,
    input  wire                        m_axis_tready
);
    // -------------------------
    // Unpack indices & gains (Channel A, 8 tones)
    // -------------------------
    wire [IDX_W -1:0] idx_a_now  [0:7];
    wire [GAIN_W-1:0] gain_a_now [0:7];

    genvar u;
    generate
        for (u=0; u<8; u=u+1) begin : UNPACK_A
            assign idx_a_now [u] = index_a_bus[(u+1)*IDX_W -1 -: IDX_W];
            assign gain_a_now[u] = gain_a_bus [(u+1)*GAIN_W-1 -: GAIN_W];
        end
    endgenerate

    // =========================================================================
    // 1) Phase-increment small table (distributed regs), capture on index change
    // =========================================================================
    localparam integer PHASE_TBL_DEPTH = (1<<IDX_W);
    reg [PHASE_W-1:0] phase_incr_tbl [0:PHASE_TBL_DEPTH-1];

    integer ii;
    initial begin
        if (PHASE_INCR_INIT != "") begin
            $readmemh(PHASE_INCR_INIT, phase_incr_tbl);
        end else begin
            for (ii=0; ii<PHASE_TBL_DEPTH; ii=ii+1) phase_incr_tbl[ii] = {PHASE_W{1'b0}};
        end
    end

    reg [IDX_W-1:0]   last_idx_a [0:7];
    reg [PHASE_W-1:0] dphi_a_reg [0:7];

    integer ca;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (ca=0; ca<8; ca=ca+1) begin
                last_idx_a[ca] <= {IDX_W{1'b0}};
                dphi_a_reg[ca] <= {PHASE_W{1'b0}};
            end
        end else begin
            for (ca=0; ca<8; ca=ca+1) begin
                if (idx_a_now[ca] != last_idx_a[ca]) begin
                    dphi_a_reg[ca] <= phase_incr_tbl[idx_a_now[ca]]; // capture once
                    last_idx_a[ca] <= idx_a_now[ca];
                end
            end
        end
    end

    // Cache gains similarly (stable for multiplier inputs)
    reg [GAIN_W-1:0] gain_a_reg [0:7];
    integer cg;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (cg=0; cg<8; cg=cg+1) gain_a_reg[cg] <= {GAIN_W{1'b0}};
        end else begin
            for (cg=0; cg<8; cg=cg+1)
                if (gain_a_now[cg] != gain_a_reg[cg]) gain_a_reg[cg] <= gain_a_now[cg];
        end
    end

    // =========================================================================
    // 2) Phase accumulators (freeze when downstream back-pressures this block)
    // =========================================================================
    // Local output register (final stage of this block)
    reg                    out_valid_r;
    reg  [DATA_WIDTH-1:0]  out_data_r;

    assign m_axis_tdata  = out_data_r;
    assign m_axis_tvalid = out_valid_r;

    // Accept/produce new sample when output register can load
    wire can_load = (~out_valid_r) || (out_valid_r && m_axis_tready);
    wire advance  = can_load;

    reg [PHASE_W-1:0] pha_a [0:7];
    integer k;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (k=0; k<8; k=k+1) pha_a[k] <= {PHASE_W{1'b0}};
        end else if (advance) begin
            for (k=0; k<8; k=k+1) pha_a[k] <= pha_a[k] + dphi_a_reg[k];
        end
        // else: hold while back-pressured
    end

    // =========================================================================
    // 3) Sine LUT BRAMs (dual-read, sync) for Channel A
    // =========================================================================
    wire [LUT_ADDR_W-1:0] lut_addr_a [0:7];
    wire signed [31:0]    wav_a      [0:7];

    generate
        for (u=0; u<8; u=u+1) begin : ADDR_A
            assign lut_addr_a[u] = pha_a[u][PHASE_W-1 -: LUT_ADDR_W];
        end
    endgenerate

    genvar l;
    generate
        for (l=0; l<4; l=l+1) begin : G_LUT_A
            lut_rom2r #(
                .LUT_DEPTH (LUT_DEPTH),
                .INIT_FILE (SINE_LUT_INIT)
            ) u_lut_a (
                .clk   (clk),
                .addr_a(lut_addr_a[2*l]),
                .dout_a(wav_a     [2*l]),
                .addr_b(lut_addr_a[2*l+1]),
                .dout_b(wav_a     [2*l+1])
            );
        end
    endgenerate
    // wav_a[*] is valid 1 cycle after lut_addr_a[*].

    // =========================================================================
    // 4) 8x multipliers (Q1.31 × Q1.FRAC_BITS -> Q1.31), AXIS handshakes
    // =========================================================================
    wire                  mul_v [0:7];
    wire signed [31:0]    mul_y [0:7];

    generate
        for (u=0; u<8; u=u+1) begin : MULS_A
            q_mul_param_hold #(
                .DATA_WIDTH (DATA_WIDTH),
                .GAIN_WIDTH (GAIN_W),
                .FRAC_BITS  (FRAC_BITS)
            ) u_mul (
                .clk           (clk),
                .rst_n         (rst_n),
                // input AXIS
                .s_axis_tvalid (advance),
                .s_axis_tready (/* unused */),
                .x_in          (wav_a[u]),
                .gain_in       (gain_a_reg[u]),
                // output AXIS
                .m_axis_tvalid (mul_v[u]),
                .m_axis_tready (can_load),
                .y_out         (mul_y[u])
            );
        end
    endgenerate

    // All multipliers share timing → valids align
    wire mul_all_valid = &{mul_v[0],mul_v[1],mul_v[2],mul_v[3],mul_v[4],mul_v[5],mul_v[6],mul_v[7]};

    // =========================================================================
    // 5) 8-input adder tree (AXIS) for Channel A
    // =========================================================================
    wire [8*DATA_WIDTH-1:0] adder_in_bus =
        { mul_y[7], mul_y[6], mul_y[5], mul_y[4],
          mul_y[3], mul_y[2], mul_y[1], mul_y[0] };

    wire                    sumA_valid;
    wire signed [31:0]      sumA_q31;

    adder_tree8_axis #(
        .DATA_WIDTH (DATA_WIDTH),
        .ACC_WIDTH  (DATA_WIDTH+3)
    ) u_adder_chA (
        .clk           (clk),
        .rst_n         (rst_n),
        // input AXIS
        .s_axis_tvalid (mul_all_valid && can_load),
        .s_axis_tready (/* unused */),
        .s_axis_tdata  (adder_in_bus),
        // output AXIS
        .m_axis_tvalid (sumA_valid),
        .m_axis_tready (can_load),
        .m_axis_tdata  (sumA_q31)
    );

    // =========================================================================
    // 6) Final output register for this block (AXIS out)
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_valid_r <= 1'b0;
            out_data_r  <= {DATA_WIDTH{1'b0}};
        end else begin
            // pop when accepted
            if (out_valid_r && m_axis_tready)
                out_valid_r <= 1'b0;

            // load new when adder output available and we can load
            if (sumA_valid && can_load) begin
                out_data_r  <= sumA_q31; // Q1.31 sum of 8 tones (Channel A)
                out_valid_r <= 1'b1;
            end
            // else: hold
        end
    end

endmodule