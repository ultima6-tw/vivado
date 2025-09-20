`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// gpio_to_axis_fifo_sync.v
// - Same-clock bridge from (wen,wdata) pulses to AXI-Stream master
// - Parameterizable small synchronous FIFO (power-of-two DEPTH recommended)
// - No backpressure to source (no busy). If FIFO is full and wen=1, set overflow.
// - Supports simultaneous push (wen) and pop (tvalid&tready) in same cycle.
// -----------------------------------------------------------------------------
module gpio_to_axis_fifo_sync #(
    parameter integer DATA_WIDTH = 32,
    parameter integer DEPTH      = 32  // choose sufficiently large (power-of-two preferred)
)(
    input  wire                   clk,
    input  wire                   rst_n,

    // Source side (GPIO-like)
    input  wire                   wen,            // 1-cycle pulse, enqueue when 1
    input  wire [DATA_WIDTH-1:0]  wdata,

    // Optional debug (no backpressure to source)
    output reg                    overflow,       // latched when write on full (sticky until reset)

    // AXI-Stream Master toward downstream
    output wire [DATA_WIDTH-1:0]  m_axis_tdata,
    output wire                   m_axis_tvalid,
    input  wire                   m_axis_tready
);
    // Address width; DEPTH may be any value, power-of-two simplifies logic
    localparam integer AW = (DEPTH <= 2)   ? 1 :
                            (DEPTH <= 4)   ? 2 :
                            (DEPTH <= 8)   ? 3 :
                            (DEPTH <= 16)  ? 4 :
                            (DEPTH <= 32)  ? 5 :
                            (DEPTH <= 64)  ? 6 :
                            (DEPTH <= 128) ? 7 :
                            (DEPTH <= 256) ? 8 : 9; // extend if needed

    reg [DATA_WIDTH-1:0] mem [0:DEPTH-1];

    reg [AW-1:0]  wptr, rptr;
    reg [AW:0]    count;       // 0..DEPTH
    
    // 位寬安全的 full/empty 定義
    wire [AW:0] depth_c = DEPTH;                 // 自動擴位到 AW+1 bits
    wire        full    = (count == depth_c);
    wire        empty   = (count == { (AW+1){1'b0} });

    // AXIS output
    assign m_axis_tvalid = ~empty;
    assign m_axis_tdata  = mem[rptr];

    // Handshakes
    wire pop  = m_axis_tvalid & m_axis_tready;   // downstream consumes
    // Only enqueue on rising edge of 'wen' (glitch-free for AXI-GPIO level writes)
    reg wen_q;
    always @(posedge clk or negedge rst_n) begin
      if (!rst_n) wen_q <= 1'b0;
      else        wen_q <= wen;
    end
    wire push = wen & ~wen_q;                              // source attempts to enqueue every wen

    // Sequential control
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wptr     <= {AW{1'b0}};
            rptr     <= {AW{1'b0}};
            count    <= { (AW+1){1'b0} };
            overflow <= 1'b0;
        end else begin
            // POP first (allows simultaneous push/pop into same location safely)
            if (pop) begin
                rptr  <= (rptr == (DEPTH-1)) ? {AW{1'b0}} : (rptr + {{(AW-1){1'b0}},1'b1});
            end

            // PUSH if wen=1
            if (push) begin
                if (!full) begin
                    mem[wptr] <= wdata;
                    wptr <= (wptr == (DEPTH-1)) ? {AW{1'b0}} : (wptr + {{(AW-1){1'b0}},1'b1});
                end else begin
                    // No backpressure path; flag overflow (drop-new policy)
                    overflow <= 1'b1;
                end
            end

            // COUNT update (handles 4 cases: 00,01,10,11)
            case ({push & ~full, pop})
                2'b10: count <= count + {{AW{1'b0}},1'b1}; // push only
                2'b01: count <= count - {{AW{1'b0}},1'b1}; // pop only
                default: /* 00 or 11 => no net change */ ;
            endcase
        end
    end
endmodule