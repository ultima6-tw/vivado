// ---------------------------------------------------------------------------
// q_mul_param_hold.v  (AXI-Stream style, 1-cycle latency)
// - Signed multiply with symmetric rounding & saturation
// - Input stream:  x_in (DATA_WIDTH, e.g. Q1.31)
//                  gain_in (GAIN_WIDTH, FRAC_BITS fraction, e.g. Q1.17)
// - Operation: x_in * gain_in  -> rounded -> arithmetic shift >>> FRAC_BITS
//              -> saturate to DATA_WIDTH
// - Handshake:
//     * s_axis_tvalid/s_axis_tready for input
//     * m_axis_tvalid/m_axis_tready for output
//   Single-stage pipeline: accept when output can load
// - Latency: 1 clock; holds output until consumed
// ---------------------------------------------------------------------------
`timescale 1ns / 1ps

module q_mul_param_hold #(
    parameter integer DATA_WIDTH = 32,                // e.g. Q1.31
    parameter integer GAIN_WIDTH = 18,                // e.g. Q1.17
    parameter integer FRAC_BITS  = 17,                // fractional bits in gain
    // Optional widen-before-multiply to match DSP width if desired
    parameter integer PROD_WIDTH = DATA_WIDTH + GAIN_WIDTH
)(
    input  wire                         clk,
    input  wire                         rst_n,

    // AXIS-like input
    input  wire                         s_axis_tvalid,
    output wire                         s_axis_tready,
    input  wire signed [DATA_WIDTH-1:0] x_in,       // Q1.31
    input  wire signed [GAIN_WIDTH-1:0] gain_in,    // Q1.17 (FRAC_BITS)

    // AXIS-like output
    output reg                          m_axis_tvalid,
    input  wire                         m_axis_tready,
    output reg  signed [DATA_WIDTH-1:0] y_out       // Q1.31
);
    // -----------------------------------------------------------------------
    // Handshake: single-stage pipeline
    // can_load == "output register available to accept a new sample"
    // -----------------------------------------------------------------------
    wire can_load      = (~m_axis_tvalid) || (m_axis_tvalid && m_axis_tready);
    assign s_axis_tready = can_load;

    // -----------------------------------------------------------------------
    // Arithmetic
    // -----------------------------------------------------------------------
    localparam integer PWIDTH = 2*PROD_WIDTH;

    // Sign-extend operands to PROD_WIDTH (optional DSP alignment)
    wire signed [PROD_WIDTH-1:0] x_ext =
        {{(PROD_WIDTH-DATA_WIDTH){x_in[DATA_WIDTH-1]}}, x_in};
    wire signed [PROD_WIDTH-1:0] g_ext =
        {{(PROD_WIDTH-GAIN_WIDTH){gain_in[GAIN_WIDTH-1]}}, gain_in};

    // Multiply: synthesis should infer DSP
    (* use_dsp = "yes" *)
    wire signed [PWIDTH-1:0] prod_full = x_ext * g_ext;

    // Symmetric rounding:
    //  positive: +2^(FRAC_BITS-1)
    //  negative: +(2^(FRAC_BITS-1)-1)  (note: add, not subtract)
    wire signed [PWIDTH-1:0] one      = {{(PWIDTH-1){1'b0}}, 1'b1};
    wire signed [PWIDTH-1:0] POS_BIAS = one <<< (FRAC_BITS-1);
    wire signed [PWIDTH-1:0] NEG_BIAS = POS_BIAS - one;

    wire signed [PWIDTH-1:0] biased   = prod_full + (prod_full[PWIDTH-1] ? NEG_BIAS : POS_BIAS);
    wire signed [PWIDTH-1:0] shifted  = biased >>> FRAC_BITS;

    // Saturation check (preserve sign-extension of DATA_WIDTH-1)
    wire need_sat =
        (shifted[PWIDTH-1:DATA_WIDTH] != { (PWIDTH-DATA_WIDTH){ shifted[DATA_WIDTH-1] } });

    wire signed [DATA_WIDTH-1:0] sat_val =
        shifted[PWIDTH-1] ? {1'b1, {(DATA_WIDTH-1){1'b0}}}   // negative overflow -> min
                          : {1'b0, {(DATA_WIDTH-1){1'b1}}};  // positive overflow -> max

    wire signed [DATA_WIDTH-1:0] y_next = need_sat ? sat_val : shifted[DATA_WIDTH-1:0];

    // -----------------------------------------------------------------------
    // Output register: load on accepted input, hold otherwise
    // -----------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_axis_tvalid <= 1'b0;
            y_out         <= {DATA_WIDTH{1'b0}};
        end else begin
            // Consume when downstream ready & we have valid
            if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
            end

            // Load new result if input handshake fires and we can load
            if (s_axis_tvalid && s_axis_tready) begin
                y_out         <= y_next;
                m_axis_tvalid <= 1'b1;
            end
            // Else: hold current y_out and m_axis_tvalid
        end
    end

    // -----------------------------------------------------------------------
    // Optional static check
    // -----------------------------------------------------------------------
    // synthesis translate_off
    initial begin
        if (FRAC_BITS < 1)
            $display("WARNING(q_mul_param_hold): FRAC_BITS < 1 may disable rounding.");
    end
    // synthesis translate_on

endmodule