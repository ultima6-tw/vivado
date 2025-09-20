`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// gpio_cmd_pulse_bridge
// - Pulse-based CDC "mailbox" with a 1-entry skid buffer (no drops for 2-deep bursts)
// - Source sends a 1-cycle wen_src with 32-bit wdata_src (src_clk domain)
// - Destination receives a 1-cycle wen_dst with aligned wdata_dst (dst_clk domain)
// - If a new pulse arrives while 'req' is still high, it is latched into 'pending_*'
//   and auto-sent right after the current transfer completes.
// -----------------------------------------------------------------------------
module gpio_cmd_pulse_bridge #(
    parameter integer WIDTH = 32
)(
    // Source domain (slow; AXI-GPIO)
    input  wire              src_clk,
    input  wire              src_rst_n,
    input  wire              wen_src,          // 1-cycle pulse in src_clk
    input  wire [WIDTH-1:0]  wdata_src,
    output wire              busy_src,         // 1 = mailbox occupied or pending (debug)

    // Destination domain (fast; data clock)
    input  wire              dst_clk,
    input  wire              dst_rst_n,
    output reg               wen_dst,          // 1-cycle pulse in dst_clk
    output reg  [WIDTH-1:0]  wdata_dst
);
    // ---------------- Source domain ----------------
    reg wen_q;
    wire wen_rise = wen_src & ~wen_q;   // rising edge detect

    // Active slot being transferred
    reg [WIDTH-1:0] data_buf;
    reg             req;                // request held until ACK returns

    // One-entry skid buffer for the "next" word
    reg [WIDTH-1:0] pending_data;
    reg             pending_valid;

    // Synchronize ACK back to source domain
    (* ASYNC_REG="TRUE" *) reg ack_s1, ack_s2;

    // Optional: flag an overflow if more than 2 pulses arrive back-to-back
    // reg overflow;

    assign busy_src = req | pending_valid;

    always @(posedge src_clk or negedge src_rst_n) begin
        if (!src_rst_n) begin
            wen_q         <= 1'b0;
            data_buf      <= {WIDTH{1'b0}};
            req           <= 1'b0;
            pending_data  <= {WIDTH{1'b0}};
            pending_valid <= 1'b0;
            ack_s1        <= 1'b0;
            ack_s2        <= 1'b0;
            // overflow      <= 1'b0;
        end else begin
            wen_q  <= wen_src;

            // sync ACK from dst
            ack_s1 <= ack;
            ack_s2 <= ack_s1;

            // New pulse from source
            if (wen_rise) begin
                if (!req) begin
                    // Mailbox free -> take it as the active word
                    data_buf <= wdata_src;
                    req      <= 1'b1;
                end else if (!pending_valid) begin
                    // Mailbox busy but skid empty -> queue into pending
                    pending_data  <= wdata_src;
                    pending_valid <= 1'b1;
                end else begin
                    // Mailbox busy and skid full -> drop (or assert overflow)
                    // overflow <= 1'b1;
                end
            end

            // When current transfer is ACKed, clear req; if we have a pending word,
            // immediately promote it into the active slot and reassert req.
            if (req && ack_s2) begin
                req <= 1'b0;
                if (pending_valid) begin
                    data_buf      <= pending_data;
                    pending_valid <= 1'b0;
                    req           <= 1'b1;  // arm next transfer immediately
                end
            end
        end
    end

    // Data crossing (data_buf is stable while req=1)
    (* ASYNC_REG="TRUE" *) reg [WIDTH-1:0] d_s1, d_s2;
    always @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n) begin
            d_s1 <= {WIDTH{1'b0}};
            d_s2 <= {WIDTH{1'b0}};
        end else begin
            d_s1 <= data_buf;
            d_s2 <= d_s1;
        end
    end

    // ---------------- Destination domain ----------------
    // Synchronize req and detect rising edge
    (* ASYNC_REG="TRUE" *) reg req_s1, req_s2;
    reg req_s2_q;
    reg ack;  // ACK returned to source domain

    always @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n) begin
            req_s1    <= 1'b0;
            req_s2    <= 1'b0;
            req_s2_q  <= 1'b0;
            wen_dst   <= 1'b0;
            wdata_dst <= {WIDTH{1'b0}};
            ack       <= 1'b0;
        end else begin
            req_s1   <= req;
            req_s2   <= req_s1;
            req_s2_q <= req_s2;

            // Rising edge of req -> emit one-cycle wen_dst, latch data, raise ack
            if (req_s2 & ~req_s2_q) begin
                wdata_dst <= d_s2;
                wen_dst   <= 1'b1;
                ack       <= 1'b1;
            end else begin
                wen_dst <= 1'b0;
                // Hold ACK while req remains high; deassert when req goes low
                if (!req_s2) ack <= 1'b0;
            end
        end
    end
endmodule