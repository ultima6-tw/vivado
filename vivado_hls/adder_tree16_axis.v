// ---------------------------------------------------------------------------
// adder_tree16_axis.v  (Verilog-2001)
// - 16-input pipelined adder tree with AXI-Stream style handshakes
// - Inputs are signed DATA_WIDTH (e.g., Q1.31) and already gain-applied
// - Throughput: 1 sample/clk when output is consumed each cycle
// - Latency   : 4 clocks (v1->v4)
// - Handshake :
//     * s_axis_tready = (~m_axis_tvalid) || (m_axis_tvalid && m_axis_tready)
//       (accept a new input exactly when we can load the output register)
// - Notes:
//     * Global pipeline enable = can_load. When stalled by downstream, the
//       whole tree holds its registers (no new input is accepted).
// ---------------------------------------------------------------------------
`timescale 1ns / 1ps

module adder_tree16_axis #(
    parameter integer DATA_WIDTH = 32,            // e.g. Q1.31
    parameter integer ACC_WIDTH  = DATA_WIDTH + 4 // 32->36 safely sums 16 terms
)(
    input  wire                         clk,
    input  wire                         rst_n,

    // AXIS-like input: 16xDATA flattened as {x15, x14, ..., x0}
    input  wire                         s_axis_tvalid,
    output wire                         s_axis_tready,
    input  wire [16*DATA_WIDTH-1:0]     s_axis_tdata,

    // AXIS-like output: saturated sum to DATA_WIDTH
    output reg                          m_axis_tvalid,
    input  wire                         m_axis_tready,
    output reg  signed [DATA_WIDTH-1:0] m_axis_tdata
);

    // -----------------------------------------------------------------------
    // Handshake policy: single "can_load" gating the entire pipeline
    // -----------------------------------------------------------------------
    wire can_load = (~m_axis_tvalid) || (m_axis_tvalid && m_axis_tready);
    assign s_axis_tready = can_load;

    // -----------------------------------------------------------------------
    // Unpack inputs
    // -----------------------------------------------------------------------
    wire signed [DATA_WIDTH-1:0] x [0:15];
    genvar ui;
    generate
        for (ui=0; ui<16; ui=ui+1) begin : UNPACK
            assign x[ui] = s_axis_tdata[(ui+1)*DATA_WIDTH-1 -: DATA_WIDTH];
        end
    endgenerate

    // -----------------------------------------------------------------------
    // Saturation helper (ACC_WIDTH -> DATA_WIDTH)
    // -----------------------------------------------------------------------
    function signed [DATA_WIDTH-1:0] sat_to_nbits;
        input signed [ACC_WIDTH-1:0] a;
    begin
        // Pass-through if upper bits are a pure sign extension of DATA_WIDTH-1
        if (a[ACC_WIDTH-1:DATA_WIDTH] == { (ACC_WIDTH-DATA_WIDTH){a[DATA_WIDTH-1]} })
            sat_to_nbits = a[DATA_WIDTH-1:0];
        else
            // Otherwise saturate to min/max
            sat_to_nbits = a[ACC_WIDTH-1] ? {1'b1, {(DATA_WIDTH-1){1'b0}}}
                                          : {1'b0, {(DATA_WIDTH-1){1'b1}}};
    end
    endfunction

    // -----------------------------------------------------------------------
    // Pipeline registers: 16→8→4→2→1 (enabled by can_load)
    // -----------------------------------------------------------------------
    reg signed [ACC_WIDTH-1:0] s1 [0:7];   // stage 1
    reg signed [ACC_WIDTH-1:0] s2 [0:3];   // stage 2
    reg signed [ACC_WIDTH-1:0] s3 [0:1];   // stage 3
    reg                        v1, v2, v3; // valid through pipeline

    integer i1;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v1 <= 1'b0;
            for (i1=0; i1<8; i1=i1+1)
                s1[i1] <= {ACC_WIDTH{1'b0}};
        end else if (can_load && s_axis_tvalid) begin
            v1 <= 1'b1;
            s1[0] <= {{(ACC_WIDTH-DATA_WIDTH){x[0][DATA_WIDTH-1]}},  x[0]}  +
                     {{(ACC_WIDTH-DATA_WIDTH){x[1][DATA_WIDTH-1]}},  x[1]};
            s1[1] <= {{(ACC_WIDTH-DATA_WIDTH){x[2][DATA_WIDTH-1]}},  x[2]}  +
                     {{(ACC_WIDTH-DATA_WIDTH){x[3][DATA_WIDTH-1]}},  x[3]};
            s1[2] <= {{(ACC_WIDTH-DATA_WIDTH){x[4][DATA_WIDTH-1]}},  x[4]}  +
                     {{(ACC_WIDTH-DATA_WIDTH){x[5][DATA_WIDTH-1]}},  x[5]};
            s1[3] <= {{(ACC_WIDTH-DATA_WIDTH){x[6][DATA_WIDTH-1]}},  x[6]}  +
                     {{(ACC_WIDTH-DATA_WIDTH){x[7][DATA_WIDTH-1]}},  x[7]};
            s1[4] <= {{(ACC_WIDTH-DATA_WIDTH){x[8][DATA_WIDTH-1]}},  x[8]}  +
                     {{(ACC_WIDTH-DATA_WIDTH){x[9][DATA_WIDTH-1]}},  x[9]};
            s1[5] <= {{(ACC_WIDTH-DATA_WIDTH){x[10][DATA_WIDTH-1]}}, x[10]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[11][DATA_WIDTH-1]}}, x[11]};
            s1[6] <= {{(ACC_WIDTH-DATA_WIDTH){x[12][DATA_WIDTH-1]}}, x[12]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[13][DATA_WIDTH-1]}}, x[13]};
            s1[7] <= {{(ACC_WIDTH-DATA_WIDTH){x[14][DATA_WIDTH-1]}}, x[14]} +
                     {{(ACC_WIDTH-DATA_WIDTH){x[15][DATA_WIDTH-1]}}, x[15]};
        end else if (can_load && !s_axis_tvalid) begin
            // bubble inserted
            v1 <= 1'b0;
        end
        // else: stall pipeline (hold values) when !can_load
    end

    integer i2;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v2 <= 1'b0;
            for (i2=0; i2<4; i2=i2+1)
                s2[i2] <= {ACC_WIDTH{1'b0}};
        end else if (can_load) begin
            v2    <= v1;
            s2[0] <= s1[0] + s1[1];
            s2[1] <= s1[2] + s1[3];
            s2[2] <= s1[4] + s1[5];
            s2[3] <= s1[6] + s1[7];
        end
        // else: stall
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v3   <= 1'b0;
            s3[0] <= {ACC_WIDTH{1'b0}};
            s3[1] <= {ACC_WIDTH{1'b0}};
        end else if (can_load) begin
            v3   <= v2;
            s3[0] <= s2[0] + s2[1];
            s3[1] <= s2[2] + s2[3];
        end
        // else: stall
    end

    // Final stage (to DATA_WIDTH) + output AXIS
    wire signed [DATA_WIDTH-1:0] sum_now = sat_to_nbits(s3[0] + s3[1]);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {DATA_WIDTH{1'b0}};
        end else begin
            // Pop current output if accepted
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            // Load new output when pipeline produces a value and we can load
            if (can_load && v3) begin
                m_axis_tdata  <= sum_now;
                m_axis_tvalid <= 1'b1;
            end
            // else: hold output registers
        end
    end

endmodule