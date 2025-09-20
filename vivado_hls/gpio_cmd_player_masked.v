`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// gpio_cmd_player_masked.v  (A/B both arbitrary index & gain; ends with COMMIT)
// - Emits gpio_wen(1-cycle) + gpio_wdata(32b) 命令序列：
//   A: INDEX[0..7] → A: GAIN[0..7] → B: INDEX[0..7] → B: GAIN[0..7] → COMMIT
// - 每個 tone 的 index / gain 都可用參數任意設定
// - 不送 SAFE；請在上層保持 commit_safe=1（或另外送 SAFE）
// - 命令格式：{CMD[31:28], SEL[27]=CH, SEL[26:24]=TONE, [23:20]=0, DATA[19:0]}
//   CMD: 1=INDEX, 2=GAIN, F=COMMIT
// -----------------------------------------------------------------------------
module gpio_cmd_player_masked #(
    parameter integer IDX_W        = 10,        // index bits (放在 DATA[19:0] 低位)
    parameter integer GAIN_W       = 18,        // gain bits  (Q1.17 放在 DATA[19:0])

    parameter integer GAP_CYCLES   = 1,         // 每筆命令間空拍（0 表示無空拍）
    parameter        AUTO_RUN      = 1,         // 上電自動開始
    parameter        LOOP_ENABLE   = 0,         // 結束後是否循環

    // ===== Channel A: 任意 index/gain（tone 0..7）=====
    parameter [IDX_W-1:0]  IDX_A_T0 = 0,
    parameter [IDX_W-1:0]  IDX_A_T1 = 1,
    parameter [IDX_W-1:0]  IDX_A_T2 = 2,
    parameter [IDX_W-1:0]  IDX_A_T3 = 3,
    parameter [IDX_W-1:0]  IDX_A_T4 = 4,
    parameter [IDX_W-1:0]  IDX_A_T5 = 5,
    parameter [IDX_W-1:0]  IDX_A_T6 = 6,
    parameter [IDX_W-1:0]  IDX_A_T7 = 7,

    parameter [GAIN_W-1:0] GAIN_A_T0 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T1 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T2 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T3 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T4 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T5 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T6 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_A_T7 = 18'h1_FFFF,

    // ===== Channel B: 任意 index/gain（tone 0..7）=====
    parameter [IDX_W-1:0]  IDX_B_T0 = 8,
    parameter [IDX_W-1:0]  IDX_B_T1 = 9,
    parameter [IDX_W-1:0]  IDX_B_T2 = 10,
    parameter [IDX_W-1:0]  IDX_B_T3 = 11,
    parameter [IDX_W-1:0]  IDX_B_T4 = 12,
    parameter [IDX_W-1:0]  IDX_B_T5 = 13,
    parameter [IDX_W-1:0]  IDX_B_T6 = 14,
    parameter [IDX_W-1:0]  IDX_B_T7 = 15,

    parameter [GAIN_W-1:0] GAIN_B_T0 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_B_T1 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_B_T2 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_B_T3 = 18'h0AAA,
    parameter [GAIN_W-1:0] GAIN_B_T4 = 18'h0AAA,
    parameter [GAIN_W-1:0] GAIN_B_T5 = 18'h0AAA,
    parameter [GAIN_W-1:0] GAIN_B_T6 = {GAIN_W{1'b0}},
    parameter [GAIN_W-1:0] GAIN_B_T7 = {GAIN_W{1'b0}}
)(
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,        // AUTO_RUN=1 時忽略

    output reg         gpio_wen,     // 1-cycle pulse
    output reg [31:0]  gpio_wdata,   // command word

    output reg         busy,
    output reg         done
);
    // ==== CMD 編碼（要和 gpio_cfg_decoder_1x32 對上）====
    localparam [3:0] CMD_IDX    = 4'h1;
    localparam [3:0] CMD_GAIN   = 4'h2;
    localparam [3:0] CMD_COMMIT = 4'hF;

    // SEL 打包： [27]=CH(0=A,1=B), [26:24]=TONE, [23:20]=0
    function [7:0] pack_sel;
        input ch;
        input [2:0] tone;
    begin
        pack_sel = {ch, tone[2:0], 4'b0000};
    end
    endfunction

    // 組 INDEX/Gain/COMMIT 指令（資料放 DATA[19:0]）
    function [31:0] make_index_word;
        input ch;                 // 0=A,1=B
        input [2:0] tone;         // 0..7
        input [IDX_W-1:0] index;
        reg [31:0] w;
    begin
        w = {CMD_IDX, pack_sel(ch, tone), 20'h0};
        w[19:0] = index[IDX_W-1:0];         // 截到 20b
        make_index_word = w;
    end
    endfunction

    function [31:0] make_gain_word;
        input ch;
        input [2:0] tone;
        input [GAIN_W-1:0] gain_q17;
        reg [31:0] w;
    begin
        w = {CMD_GAIN, pack_sel(ch, tone), 20'h0};
        w[19:0] = gain_q17[GAIN_W-1:0];     // 截到 20b（Q1.17 fits）
        make_gain_word = w;
    end
    endfunction

    // 一些工具/合成器版本需要 function 一定要有至少一個 input
    function [31:0] make_commit_word;
        input dummy;
        reg [31:0] w;
    begin
        w = {CMD_COMMIT, 8'h00, 20'h0};     // SEL=0, DATA=0
        make_commit_word = w;
    end
    endfunction

    // A/B per-tone 取參數
    function [IDX_W-1:0] idx_a_of;
        input [2:0] tone;
    begin
        case (tone)
            3'd0: idx_a_of = IDX_A_T0;
            3'd1: idx_a_of = IDX_A_T1;
            3'd2: idx_a_of = IDX_A_T2;
            3'd3: idx_a_of = IDX_A_T3;
            3'd4: idx_a_of = IDX_A_T4;
            3'd5: idx_a_of = IDX_A_T5;
            3'd6: idx_a_of = IDX_A_T6;
            default: idx_a_of = IDX_A_T7;
        endcase
    end
    endfunction

    function [GAIN_W-1:0] gain_a_of;
        input [2:0] tone;
    begin
        case (tone)
            3'd0: gain_a_of = GAIN_A_T0;
            3'd1: gain_a_of = GAIN_A_T1;
            3'd2: gain_a_of = GAIN_A_T2;
            3'd3: gain_a_of = GAIN_A_T3;
            3'd4: gain_a_of = GAIN_A_T4;
            3'd5: gain_a_of = GAIN_A_T5;
            3'd6: gain_a_of = GAIN_A_T6;
            default: gain_a_of = GAIN_A_T7;
        endcase
    end
    endfunction

    function [IDX_W-1:0] idx_b_of;
        input [2:0] tone;
    begin
        case (tone)
            3'd0: idx_b_of = IDX_B_T0;
            3'd1: idx_b_of = IDX_B_T1;
            3'd2: idx_b_of = IDX_B_T2;
            3'd3: idx_b_of = IDX_B_T3;
            3'd4: idx_b_of = IDX_B_T4;
            3'd5: idx_b_of = IDX_B_T5;
            3'd6: idx_b_of = IDX_B_T6;
            default: idx_b_of = IDX_B_T7;
        endcase
    end
    endfunction

    function [GAIN_W-1:0] gain_b_of;
        input [2:0] tone;
    begin
        case (tone)
            3'd0: gain_b_of = GAIN_B_T0;
            3'd1: gain_b_of = GAIN_B_T1;
            3'd2: gain_b_of = GAIN_B_T2;
            3'd3: gain_b_of = GAIN_B_T3;
            3'd4: gain_b_of = GAIN_B_T4;
            3'd5: gain_b_of = GAIN_B_T5;
            3'd6: gain_b_of = GAIN_B_T6;
            default: gain_b_of = GAIN_B_T7;
        endcase
    end
    endfunction

    // FSM 狀態
    localparam [2:0] S_IDLE    = 3'd0,
                     S_IDX_A   = 3'd1,
                     S_GAIN_A  = 3'd2,
                     S_IDX_B   = 3'd3,
                     S_GAIN_B  = 3'd4,
                     S_COMMIT  = 3'd5,
                     S_GAP     = 3'd6,
                     S_DONE    = 3'd7;

    reg [2:0] state, nstate;
    reg [2:0] after_gap;                 // GAP 結束後要跳往的狀態
    reg [2:0] tone_inx_a, tone_inx_b, tone_gain_a, tone_gain_b;

    // GAP 計數位寬（避免 GAP_CYCLES=0 位寬為負）
    localparam integer GCW = (GAP_CYCLES==0) ? 1 : $clog2(GAP_CYCLES+1);
    reg [GCW-1:0] gap_cnt;

    wire start_req = AUTO_RUN ? (state==S_IDLE) : start;

    // Next-state
    always @* begin
        nstate = state;
        case (state)
            S_IDLE:    if (start_req)          nstate = S_IDX_A;
            S_IDX_A:   if (tone_inx_a==3'd7)       nstate = (GAP_CYCLES==0 ? S_GAIN_A : S_GAP);
                        else                    nstate = (GAP_CYCLES==0 ? S_IDX_A  : S_GAP);
            S_GAIN_A:  if (tone_gain_a==3'd7)       nstate = (GAP_CYCLES==0 ? S_IDX_B  : S_GAP);
                        else                    nstate = (GAP_CYCLES==0 ? S_GAIN_A : S_GAP);
            S_IDX_B:   if (tone_inx_b==3'd7)       nstate = (GAP_CYCLES==0 ? S_GAIN_B : S_GAP);
                        else                    nstate = (GAP_CYCLES==0 ? S_IDX_B  : S_GAP);
            S_GAIN_B:  if (tone_gain_b==3'd7)       nstate = (GAP_CYCLES==0 ? S_COMMIT : S_GAP);
                        else                    nstate = (GAP_CYCLES==0 ? S_GAIN_B : S_GAP);
            S_COMMIT:                          nstate = (LOOP_ENABLE ? S_GAP : S_DONE);
            S_GAP:     if (gap_cnt==0)         nstate = after_gap;
            S_DONE:    if (LOOP_ENABLE)        nstate = S_IDX_A;
                        else if (!AUTO_RUN && start) nstate = S_IDX_A;
            default:                           nstate = S_IDLE;
        endcase
    end

    // Seq + 輸出
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            gpio_wen   <= 1'b0;
            gpio_wdata <= 32'h0;
            busy       <= 1'b0;
            done       <= 1'b0;
            tone_inx_a <= 3'd0;
            tone_inx_b <= 3'd0;
            tone_gain_a <= 3'd0;
            tone_gain_b <= 3'd0;
            after_gap  <= S_IDLE;
            gap_cnt    <= {GCW{1'b0}};
        end else begin
            state    <= nstate;
            gpio_wen <= 1'b0; // default

            case (state)
                S_IDLE: begin
                    busy      <= 1'b0;
                    done      <= 1'b0;
                    tone_inx_a <= 3'd0;
                    tone_inx_b <= 3'd0;
                    tone_gain_a <= 3'd0;
                    tone_gain_b <= 3'd0;
                    gap_cnt   <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                    after_gap <= S_IDX_A;
                    if (start_req) begin
                        busy       <= 1'b1;
                        gpio_wdata <= make_index_word(1'b0, 3'b000, idx_a_of(3'b000));
                        gpio_wen   <= 1'b1;
                        tone_inx_a     <= 3'd1;
                        after_gap  <= S_IDX_A;
                        gap_cnt    <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                    end
                end

                S_IDX_A: begin
                    busy       <= 1'b1;
                    gpio_wdata <= make_index_word(1'b0, tone_inx_a, idx_a_of(tone_inx_a));
                    gpio_wen   <= 1'b1;
                    if (tone_inx_a != 3'd7) begin
                        tone_inx_a    <= tone_inx_a + 1;
                        after_gap <= S_IDX_A;
                    end else begin
                        after_gap <= S_GAIN_A;
                    end
                    gap_cnt <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                end

                S_GAIN_A: begin
                    busy       <= 1'b1;
                    gpio_wdata <= make_gain_word(1'b0, tone_gain_a, gain_a_of(tone_gain_a));
                    gpio_wen   <= 1'b1;
                    if (tone_gain_a != 3'd7) begin
                        tone_gain_a    <= tone_gain_a + 1;
                        after_gap <= S_GAIN_A;
                    end else begin
                        after_gap <= S_IDX_B;
                    end
                    gap_cnt <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                end

                S_IDX_B: begin
                    busy       <= 1'b1;
                    gpio_wdata <= make_index_word(1'b1, tone_inx_b, idx_b_of(tone_inx_b));
                    gpio_wen   <= 1'b1;
                    if (tone_inx_b != 3'd7) begin
                        tone_inx_b    <= tone_inx_b + 1;
                        after_gap <= S_IDX_B;
                    end else begin
                        after_gap <= S_GAIN_B;
                    end
                    gap_cnt <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                end

                S_GAIN_B: begin
                    busy       <= 1'b1;
                    gpio_wdata <= make_gain_word(1'b1, tone_gain_b, gain_b_of(tone_gain_b));
                    gpio_wen   <= 1'b1;
                    if (tone_gain_b != 3'd7) begin
                        tone_gain_b    <= tone_gain_b + 1;
                        after_gap <= S_GAIN_B;
                    end else begin
                        after_gap <= S_COMMIT;
                    end
                    gap_cnt <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                end

                S_COMMIT: begin
                    busy       <= 1'b1;
                    gpio_wdata <= make_commit_word(1'b0); // 保留 dummy 容許舊合成器
                    gpio_wen   <= 1'b1;
                    after_gap  <= LOOP_ENABLE ? S_IDX_A : S_DONE;
                    gap_cnt    <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                end

                S_GAP: begin
                    busy <= 1'b1;
                    if (gap_cnt != {GCW{1'b0}}) gap_cnt <= gap_cnt - {{(GCW-1){1'b0}},1'b1};
                end

                S_DONE: begin
                    busy <= 1'b0;
                    done <= 1'b1;
                    if (LOOP_ENABLE || (!AUTO_RUN && start)) begin
                        done       <= 1'b0;
                        busy       <= 1'b1;
                        tone_inx_a     <= 3'd0;
                        tone_inx_b     <= 3'd0;
                        tone_gain_a    <= 3'd0;
                        tone_gain_b    <= 3'd0;
                        gpio_wdata <= make_index_word(1'b0, 3'b000, idx_a_of(3'b000));
                        gpio_wen   <= 1'b1;
                        after_gap  <= S_IDX_A;
                        gap_cnt    <= (GAP_CYCLES==0) ? {GCW{1'b0}} : GAP_CYCLES[GCW-1:0];
                    end
                end
            endcase
        end
    end
endmodule