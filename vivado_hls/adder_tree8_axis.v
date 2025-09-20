// ---------------------------------------------------------------------------
// adder_tree8_axis.v  (Verilog-2001)
// - 8-input pipelined adder tree with AXI-Stream style handshakes
// - Inputs are signed DATA_WIDTH (e.g., Q1.31) and already gain-applied
// - Throughput: 1 sample/clk when output is consumed each cycle
// - Latency   : 3 clocks (v1->v3)
// - Handshake :
//     * s_axis_tready = (~m_axis_tvalid) || (m_axis_tvalid && m_axis_tready)
//       (accept a new input exactly when we can load the output register)
// ---------------------------------------------------------------------------
`timescale 1ns / 1ps

module adder_tree8_axis #(
    parameter integer DATA_WIDTH = 32,            // e.g. Q1.31
    parameter integer ACC_WIDTH  = DATA_WIDTH + 3 // 32->35 safely sums 8 terms
)(
    input  wire                         clk,
    input  wire                         rst_n,

    // AXIS-like input: 8xDATA flattened as {x7, x6, ..., x0}
    input  wire                         s_axis_tvalid,
    output wire                         s_axis_tready,
    input  wire [8*DATA_WIDTH-1:0]      s_axis_tdata,

    // AXIS-like output: saturated sum to DATA_WIDTH
    output reg                          m_axis_tvalid,
    input  wire                         m_axis_tready,
    output reg  signed [DATA_WIDTH-1:0] m_axis_tdata
);
    // -------------------------
    // Handshake policy
    // -------------------------
    wire can_load = (~m_axis_tvalid) || (m_axis_tvalid && m_axis_tready);
    assign s_axis_tready = can_load;

    // -------------------------
    // Unpack inputs
    // -------------------------
    wire signed [DATA_WIDTH-1:0] x [0:7];
    genvar ui;
    generate
        for (ui=0; ui<8; ui=ui+1) begin : UNPACK
            assign x[ui] = s_axis_tdata[(ui+1)*DATA_WIDTH-1 -: DATA_WIDTH];
        end
    endgenerate

    // -------------------------
    // Saturation ACC_WIDTH -> DATA_WIDTH
    // -------------------------
    function signed [DATA_WIDTH-1:0] sat_to_nbits;
        input signed [ACC_WIDTH-1:0] a;
    begin
        if (a[ACC_WIDTH-1:DATA_WIDTH] == { (ACC_WIDTH-DATA_WIDTH){a[DATA_WIDTH-1]} })
            sat_to_nbits = a[DATA_WIDTH-1:0];
        else
            sat_to_nbits = a[ACC_WIDTH-1] ? {1'b1, {(DATA_WIDTH-1){1'b0}}}
                                          : {1'b0, {(DATA_WIDTH-1){1'b1}}};
    end
    endfunction

    // -------------------------
    // Pipeline: 8→4→2→1 (3 stages)
    // -------------------------
    reg signed [ACC_WIDTH-1:0] s1 [0:3];  // stage 1
    reg signed [ACC_WIDTH-1:0] s2 [0:1];  // stage 2
    reg                        v1, v2;    // valids

    // Stage 1 (pairwise: 8 -> 4)
    integer i1;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v1 <= 1'b0;
            for (i1=0; i1<4; i1=i1+1)
                s1[i1] <= {ACC_WIDTH{1'b0}};
        end else if (can_load && s_axis_tvalid) begin
            v1    <= 1'b1;
            s1[0] <= {{(ACC_WIDTH-DATA_WIDTH){x[0][DATA_WIDTH-1]}}, x[0]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[1][DATA_WIDTH-1]}}, x[1]};
            s1[1] <= {{(ACC_WIDTH-DATA_WIDTH){x[2][DATA_WIDTH-1]}}, x[2]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[3][DATA_WIDTH-1]}}, x[3]};
            s1[2] <= {{(ACC_WIDTH-DATA_WIDTH){x[4][DATA_WIDTH-1]}}, x[4]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[5][DATA_WIDTH-1]}}, x[5]};
            s1[3] <= {{(ACC_WIDTH-DATA_WIDTH){x[6][DATA_WIDTH-1]}}, x[6]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[7][DATA_WIDTH-1]}}, x[7]};
        end else if (can_load && !s_axis_tvalid) begin
            v1 <= 1'b0; // bubble
        end
        // else: stall all when !can_load
    end

    // Stage 2 (4 -> 2)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v2   <= 1'b0;
            s2[0] <= {ACC_WIDTH{1'b0}};
            s2[1] <= {ACC_WIDTH{1'b0}};
        end else if (can_load) begin
            v2   <= v1;
            s2[0] <= s1[0] + s1[1];
            s2[1] <= s1[2] + s1[3];
        end
    end

    // Stage 3 (2 -> 1) + saturation + output
    wire signed [DATA_WIDTH-1:0] sum_now = sat_to_nbits(s2[0] + s2[1]);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {DATA_WIDTH{1'b0}};
        end else begin
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            if (can_load && v2) begin
                m_axis_tdata  <= sum_now;
                m_axis_tvalid <= 1'b1;
            end
        end
    end

endmodule