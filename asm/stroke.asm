; =============================================================================
;  stroke.asm  -  32-bit x86 / x87 FPU stroke backend (NASM)
;
;  Built as stroke.dll (win32 object format) and called from the C++/SDL2
;  frontend through the native C calling convention (__cdecl):
;    - arguments pushed right-to-left, caller cleans the stack
;    - EAX/ECX/EDX volatile, EBX/ESI/EDI/EBP preserved
;    - float results returned in ST(0), FPU stack otherwise left empty
;
;  Data layout shared with C++:
;    struct Point { float x; float y; };      ; 8 bytes, packed
;
;  Exported routines
;    Sampling (interrupt-style, O(1), no allocation)
;      int   sample_isr(int x, int y)                  ; 1 stored, 0 dropped
;      int   sample_drain(Point* out, int max)         ; returns count copied
;      void  sample_reset(void)
;      int   sample_overflow(void)                     ; dropped-sample count
;    Recognizer pipeline (x87)
;      float stroke_path_length(const Point* p, int n)
;      void  stroke_centroid(const Point* p, int n, Point* out)
;      int   stroke_resample(const Point* in, int n, Point* out)   ; -> 64 pts
;      float stroke_indicative_angle(const Point* p, int n)        ; radians
;      void  stroke_rotate(Point* p, int n, float theta)           ; about centroid
;      float stroke_score(const Point* cand, const Point* tmpl, int n) ; 0..100
;      float stroke_evaluate(const Point* raw, int n,
;                            const Point* tmplRaw, int m)          ; full pipeline
; =============================================================================

        bits 32
        cpu  686                        ; fcomi / fcmov (P6 x87)

%ifidn __OUTPUT_FORMAT__, win32
  %define SYM(n) _ %+ n                 ; cdecl decoration on Win32
%else
  %define SYM(n) n                      ; elf32 (Linux unit tests)
%endif

%macro FUNC 1
        global SYM(%1)
SYM(%1):
%endmacro

STROKE_N        equ 64                  ; resampled point count (N = 64)
RING_SIZE       equ 8192                ; must be a power of two
RING_MASK       equ RING_SIZE - 1

; -----------------------------------------------------------------------------
section .data
        align 4
k_nm1           dd STROKE_N - 1         ; N - 1, integer for fidiv
k_hundred       dd 100                  ; integer for fimul
k_half          dd 0.5
k_max_tilt      dd 0.3926991            ; pi/8 = 22.5 deg rotation tolerance

; -----------------------------------------------------------------------------
section .bss
        alignb 16
ring_buf        resb RING_SIZE * 8      ; Point ring buffer
ring_head       resd 1                  ; producer index (written by ISR)
ring_tail       resd 1                  ; consumer index (written by drain)
ring_dropped    resd 1
        alignb 16
buf_cand        resb STROKE_N * 8       ; scratch for stroke_evaluate
buf_tmpl        resb STROKE_N * 8

; -----------------------------------------------------------------------------
section .text

%ifidn __OUTPUT_FORMAT__, win32
; DLL entry point. The DLL is linked without a C runtime, so this is the
; whole of process/thread attach handling.
        global _DllMain@12
_DllMain@12:
        mov     eax, 1
        ret     12
%endif

; =============================================================================
;  SAMPLING  -  interrupt-style coordinate capture
;
;  sample_isr is hooked into the frontend's input event path and runs once
;  per mouse report. It is constant-time: one masked store into a fixed ring,
;  no branches on buffer contents, no allocation, no FPU state left behind.
;  Single producer (ISR) / single consumer (drain).
; =============================================================================

; int sample_isr(int x, int y)
FUNC sample_isr
        mov     eax, [ring_head]
        mov     edx, eax
        sub     edx, [ring_tail]
        cmp     edx, RING_SIZE
        jae     .full
        mov     ecx, eax
        and     ecx, RING_MASK
        fild    dword [esp+4]                   ; x
        fstp    dword [ring_buf + ecx*8]
        fild    dword [esp+8]                   ; y
        fstp    dword [ring_buf + ecx*8 + 4]
        inc     eax
        mov     [ring_head], eax                ; publish after the store
        mov     eax, 1
        ret
.full:
        inc     dword [ring_dropped]
        xor     eax, eax
        ret

; int sample_drain(Point* out, int max)
FUNC sample_drain
        push    esi
        push    edi
        push    ebx
        mov     edi, [esp+16]                   ; out
        mov     ebx, [esp+20]                   ; max
        mov     esi, [ring_tail]
        mov     edx, [ring_head]
        xor     eax, eax                        ; copied
.loop:
        cmp     esi, edx
        je      .done
        cmp     eax, ebx
        jge     .done
        mov     ecx, esi
        and     ecx, RING_MASK
        push    edx
        mov     edx, [ring_buf + ecx*8]
        mov     [edi + eax*8], edx
        mov     edx, [ring_buf + ecx*8 + 4]
        mov     [edi + eax*8 + 4], edx
        pop     edx
        inc     esi
        inc     eax
        jmp     .loop
.done:
        mov     [ring_tail], esi
        pop     ebx
        pop     edi
        pop     esi
        ret

; void sample_reset(void)
FUNC sample_reset
        mov     eax, [ring_head]
        mov     [ring_tail], eax
        mov     dword [ring_dropped], 0
        ret

; int sample_overflow(void)
FUNC sample_overflow
        mov     eax, [ring_dropped]
        ret

; =============================================================================
;  RECOGNIZER  -  x87 FPU pipeline
; =============================================================================

; float stroke_path_length(const Point* p, int n)
;   Sum of Euclidean distances between consecutive points.
FUNC stroke_path_length
        push    ebp
        mov     ebp, esp
        push    esi
        mov     esi, [ebp+8]
        mov     ecx, [ebp+12]
        fldz                                    ; L
        dec     ecx
        jle     .done
.loop:
        fld     dword [esi+8]
        fsub    dword [esi]                     ; dx, L
        fmul    st0, st0                        ; dx^2, L
        fld     dword [esi+12]
        fsub    dword [esi+4]                   ; dy, dx^2, L
        fmul    st0, st0
        faddp   st1, st0                        ; dx^2+dy^2, L
        fsqrt
        faddp   st1, st0                        ; L += d
        add     esi, 8
        dec     ecx
        jnz     .loop
.done:
        pop     esi
        pop     ebp
        ret

; void stroke_centroid(const Point* p, int n, Point* out)
;   Spatial normalization: centre = (sum of N coordinates) / N.
FUNC stroke_centroid
        push    ebp
        mov     ebp, esp
        push    esi
        push    edi
        mov     esi, [ebp+8]
        mov     ecx, [ebp+12]
        mov     edi, [ebp+16]
        test    ecx, ecx
        jle     .empty
        fldz                                    ; sumY
        fldz                                    ; sumX, sumY
.loop:
        fadd    dword [esi]                     ; sumX += x
        fxch
        fadd    dword [esi+4]                   ; sumY += y
        fxch
        add     esi, 8
        dec     ecx
        jnz     .loop
        fild    dword [ebp+12]                  ; n, sumX, sumY
        fdiv    st1, st0                        ; n, cx, sumY
        fdivp   st2, st0                        ; cx, cy
        fstp    dword [edi]
        fstp    dword [edi+4]
        jmp     .out
.empty:
        xor     eax, eax
        mov     [edi], eax
        mov     [edi+4], eax
.out:
        pop     edi
        pop     esi
        pop     ebp
        ret

; int stroke_resample(const Point* in, int n, Point* out)
;   Speed normalization: re-places the trail as N = 64 points spaced evenly
;   along its arc length, so fast and slow drawing produce the same density.
;   out must hold STROKE_N points. Returns points written (64, or 0 if n < 1).
;
;   locals (doubles):  [ebp-8] I   interval length
;                      [ebp-16] D  distance accumulated since last emit
;                      [ebp-24] px, [ebp-32] py   previous point
;                      [ebp-40] d  current segment length
FUNC stroke_resample
        push    ebp
        mov     ebp, esp
        sub     esp, 40
        push    ebx
        push    esi
        push    edi
        mov     esi, [ebp+8]
        mov     edi, [ebp+16]
        xor     eax, eax
        cmp     dword [ebp+12], 1
        jl      .done

        push    dword [ebp+12]
        push    esi
        call    SYM(stroke_path_length)         ; st0 = L
        add     esp, 8
        fidiv   dword [k_nm1]                   ; I = L / (N-1)
        fstp    qword [ebp-8]
        fldz
        fstp    qword [ebp-16]                  ; D = 0

        fld     dword [esi]                     ; out[0] = prev = in[0]
        fst     qword [ebp-24]
        fstp    dword [edi]
        fld     dword [esi+4]
        fst     qword [ebp-32]
        fstp    dword [edi+4]

        mov     ebx, 1                          ; k  (points emitted)
        mov     edx, 1                          ; i  (input index)
.loop:
        cmp     edx, [ebp+12]
        jge     .pad
        cmp     ebx, STROKE_N
        jge     .finish

        fld     dword [esi+edx*8]
        fsub    qword [ebp-24]                  ; dx
        fld     dword [esi+edx*8+4]
        fsub    qword [ebp-32]                  ; dy, dx
        fld     st1
        fmul    st0, st0                        ; dx^2, dy, dx
        fld     st1
        fmul    st0, st0                        ; dy^2, dx^2, dy, dx
        faddp   st1, st0
        fsqrt                                   ; d, dy, dx
        fst     qword [ebp-40]

        fldz
        fcomip  st0, st1                        ; 0 >= d ?  (zero-length seg)
        jae     .skip

        fadd    qword [ebp-16]                  ; D+d, dy, dx
        fld     qword [ebp-8]
        fcomip  st0, st1                        ; I > D+d ?
        ja      .accumulate

        ; interval reached: q = prev + ((I - D) / d) * (in[i] - prev)
        fstp    st0                             ; dy, dx
        fld     qword [ebp-8]
        fsub    qword [ebp-16]
        fdiv    qword [ebp-40]                  ; t, dy, dx
        fmul    st2, st0                        ; t, dy, t*dx
        fmulp   st1, st0                        ; t*dy, t*dx
        fadd    qword [ebp-32]                  ; qy, t*dx
        fxch
        fadd    qword [ebp-24]                  ; qx, qy
        fst     qword [ebp-24]
        fstp    dword [edi+ebx*8]
        fst     qword [ebp-32]
        fstp    dword [edi+ebx*8+4]
        fldz
        fstp    qword [ebp-16]                  ; D = 0
        inc     ebx
        jmp     .loop                           ; same i: q becomes prev

.accumulate:                                    ; D+d, dy, dx
        fstp    qword [ebp-16]                  ; D += d
        fstp    st0
        fstp    st0
        jmp     .advance
.skip:                                          ; d, dy, dx
        fstp    st0
        fstp    st0
        fstp    st0
.advance:
        fld     dword [esi+edx*8]
        fstp    qword [ebp-24]
        fld     dword [esi+edx*8+4]
        fstp    qword [ebp-32]
        inc     edx
        jmp     .loop

.pad:                                           ; rounding shortfall: repeat last
        mov     ecx, [ebp+12]
        dec     ecx
        mov     eax, [esi+ecx*8]
        mov     ecx, [esi+ecx*8+4]
.padloop:
        cmp     ebx, STROKE_N
        jge     .finish
        mov     [edi+ebx*8], eax
        mov     [edi+ebx*8+4], ecx
        inc     ebx
        jmp     .padloop
.finish:
        mov     eax, ebx
.done:
        pop     edi
        pop     esi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret

; float stroke_indicative_angle(const Point* p, int n)
;   Angle from the first point to the centroid: atan2(cy - y0, cx - x0),
;   computed with FPATAN (ST1 = atan(ST1/ST0) with quadrant, pop).
FUNC stroke_indicative_angle
        push    ebp
        mov     ebp, esp
        sub     esp, 8                          ; Point c
        push    esi
        mov     esi, [ebp+8]
        cmp     dword [ebp+12], 1
        jl      .zero
        lea     eax, [ebp-8]
        push    eax
        push    dword [ebp+12]
        push    esi
        call    SYM(stroke_centroid)
        add     esp, 12
        fld     dword [ebp-4]
        fsub    dword [esi+4]                   ; cy - y0
        fld     dword [ebp-8]
        fsub    dword [esi]                     ; cx - x0, cy - y0
        fpatan                                  ; theta
        jmp     .out
.zero:
        fldz
.out:
        pop     esi
        mov     esp, ebp
        pop     ebp
        ret

; void stroke_rotate(Point* p, int n, float theta)
;   Rotates every point by theta about the centroid. The centroid itself is
;   unchanged, so an off-centre drawing stays off-centre and is penalised
;   later by the distance comparison.
;
;   cos/sin come from FPTAN using the half-angle identities
;       t = tan(theta/2),  cos = (1-t^2)/(1+t^2),  sin = 2t/(1+t^2)
;   which keeps the argument inside (-pi/2, pi/2] for theta in [-pi, pi].
;
;   locals (doubles): [ebp-8] cos  [ebp-16] sin  [ebp-24] cx  [ebp-32] cy
;                     [ebp-40] Point (float) centroid
FUNC stroke_rotate
        push    ebp
        mov     ebp, esp
        sub     esp, 40
        push    esi
        mov     esi, [ebp+8]
        cmp     dword [ebp+12], 1
        jl      .out

        lea     eax, [ebp-40]
        push    eax
        push    dword [ebp+12]
        push    esi
        call    SYM(stroke_centroid)
        add     esp, 12
        fld     dword [ebp-40]
        fstp    qword [ebp-24]
        fld     dword [ebp-36]
        fstp    qword [ebp-32]

        fld     dword [ebp+16]                  ; theta
        fmul    dword [k_half]                  ; theta/2
        fptan                                   ; 1.0, t
        fstp    st0                             ; t
        fld     st0
        fmul    st0, st0                        ; t^2, t
        fld1
        fadd    st0, st1                        ; 1+t^2, t^2, t
        fld1
        fsub    st0, st2                        ; 1-t^2, 1+t^2, t^2, t
        fdiv    st0, st1                        ; cos, 1+t^2, t^2, t
        fstp    qword [ebp-8]                   ; 1+t^2, t^2, t
        fxch    st2                             ; t, t^2, 1+t^2
        fadd    st0, st0                        ; 2t, t^2, 1+t^2
        fdiv    st0, st2                        ; sin, t^2, 1+t^2
        fstp    qword [ebp-16]
        fstp    st0
        fstp    st0

        mov     ecx, [ebp+12]
.loop:
        fld     dword [esi]
        fsub    qword [ebp-24]                  ; dx
        fld     dword [esi+4]
        fsub    qword [ebp-32]                  ; dy, dx
        fld     st1
        fmul    qword [ebp-8]                   ; dx*c, dy, dx
        fld     st1
        fmul    qword [ebp-16]                  ; dy*s, dx*c, dy, dx
        fsubp   st1, st0                        ; dx*c - dy*s, dy, dx
        fadd    qword [ebp-24]
        fstp    dword [esi]                     ; x' ; stack: dy, dx
        fmul    qword [ebp-8]                   ; dy*c, dx
        fxch
        fmul    qword [ebp-16]                  ; dx*s, dy*c
        faddp   st1, st0
        fadd    qword [ebp-32]
        fstp    dword [esi+4]                   ; y'
        add     esi, 8
        dec     ecx
        jnz     .loop
.out:
        pop     esi
        mov     esp, ebp
        pop     ebp
        ret

; float stroke_score(const Point* cand, const Point* tmpl, int n)
;   Template comparison: average Euclidean distance between corresponding
;   points, expressed against the bounding box of the drawing (candidate and
;   template together):
;       score = 100 * (1 - avg / (0.5 * diagonal)),  clamped to [0, 100]
;
;   locals (floats): [ebp-4] minx [ebp-8] maxx [ebp-12] miny [ebp-16] maxy
FUNC stroke_score
        push    ebp
        mov     ebp, esp
        sub     esp, 16
        push    esi
        push    edi
        mov     esi, [ebp+8]
        mov     edi, [ebp+12]
        mov     ecx, [ebp+16]
        cmp     ecx, 1
        jl      .zero

        ; ---- bounding box over both arrays ----
        mov     eax, [esi]
        mov     [ebp-4], eax
        mov     [ebp-8], eax
        mov     eax, [esi+4]
        mov     [ebp-12], eax
        mov     [ebp-16], eax

%macro BBOX_PT 1                                ; %1 = address of a Point
        fld     dword [ebp-4]
        fld     dword [%1]
        fcomi   st0, st1
        fcmovnb st0, st1                        ; min(x, minx)
        fstp    dword [ebp-4]
        fstp    st0
        fld     dword [ebp-8]
        fld     dword [%1]
        fcomi   st0, st1
        fcmovb  st0, st1                        ; max(x, maxx)
        fstp    dword [ebp-8]
        fstp    st0
        fld     dword [ebp-12]
        fld     dword [%1+4]
        fcomi   st0, st1
        fcmovnb st0, st1
        fstp    dword [ebp-12]
        fstp    st0
        fld     dword [ebp-16]
        fld     dword [%1+4]
        fcomi   st0, st1
        fcmovb  st0, st1
        fstp    dword [ebp-16]
        fstp    st0
%endmacro

        xor     edx, edx
.bbox:
        BBOX_PT esi + edx*8
        BBOX_PT edi + edx*8
        inc     edx
        cmp     edx, ecx
        jl      .bbox

        ; ---- average point-to-point distance ----
        fldz                                    ; sum
        xor     edx, edx
.dist:
        fld     dword [esi+edx*8]
        fsub    dword [edi+edx*8]
        fmul    st0, st0
        fld     dword [esi+edx*8+4]
        fsub    dword [edi+edx*8+4]
        fmul    st0, st0
        faddp   st1, st0
        fsqrt
        faddp   st1, st0
        inc     edx
        cmp     edx, ecx
        jl      .dist
        fidiv   dword [ebp+16]                  ; avg

        ; ---- half diagonal of the box ----
        fld     dword [ebp-8]
        fsub    dword [ebp-4]
        fmul    st0, st0                        ; w^2, avg
        fld     dword [ebp-16]
        fsub    dword [ebp-12]
        fmul    st0, st0
        faddp   st1, st0
        fsqrt
        fmul    dword [k_half]                  ; half, avg
        fldz
        fcomip  st0, st1                        ; 0 >= half ?  degenerate box
        jae     .degenerate

        fdivp   st1, st0                        ; avg/half
        fld1
        fsubrp  st1, st0                        ; 1 - avg/half
        fimul   dword [k_hundred]
        fldz                                    ; clamp low
        fcomi   st0, st1
        fcmovb  st0, st1                        ; max(0, s)
        fstp    st1
        fild    dword [k_hundred]               ; clamp high
        fcomi   st0, st1
        fcmovnb st0, st1                        ; min(100, s)
        fstp    st1
        jmp     .out
.degenerate:                                    ; half, avg
        fstp    st0
        fstp    st0
.zero:
        fldz
.out:
        pop     edi
        pop     esi
        mov     esp, ebp
        pop     ebp
        ret

; float stroke_evaluate(const Point* raw, int n, const Point* tmplRaw, int m)
;   Full pipeline, called once per completed (merged) attempt:
;     1. resample candidate and template to N = 64
;     2. indicative angle of each (first point -> centroid); rotate the
;        candidate about its own centroid toward the template's starting
;        direction. The correction is limited to +/- MAX_TILT so a slightly
;        tilted letter is forgiven, but a stroke drawn backwards or started
;        from the wrong end is NOT rotated into a match: directionality
;        stays part of the score.
;     3. distance score against the shared bounding box
;   Returns 0 when either path has fewer than 2 points.
;
;   locals: [ebp-4] float theta   [ebp-8] float theta_cand
FUNC stroke_evaluate
        push    ebp
        mov     ebp, esp
        sub     esp, 8
        cmp     dword [ebp+12], 2
        jl      .zero
        cmp     dword [ebp+20], 2
        jl      .zero

        push    buf_cand
        push    dword [ebp+12]
        push    dword [ebp+8]
        call    SYM(stroke_resample)
        add     esp, 12
        push    buf_tmpl
        push    dword [ebp+20]
        push    dword [ebp+16]
        call    SYM(stroke_resample)
        add     esp, 12

        push    dword STROKE_N
        push    buf_cand
        call    SYM(stroke_indicative_angle)
        add     esp, 8
        fstp    dword [ebp-8]                   ; theta_cand
        push    dword STROKE_N
        push    buf_tmpl
        call    SYM(stroke_indicative_angle)
        add     esp, 8                          ; theta_tmpl

        ; delta = theta_tmpl - theta_cand, wrapped to [-pi, pi] with FPREM1
        fsub    dword [ebp-8]                   ; delta
        fldpi
        fadd    st0, st0                        ; 2pi, delta
        fxch                                    ; delta, 2pi
.wrap:  fprem1
        fnstsw  ax
        test    ah, 4                           ; C2 set -> reduction incomplete
        jnz     .wrap
        fstp    st1                             ; delta in [-pi, pi]

        ; clamp to [-MAX_TILT, +MAX_TILT]
        fld     dword [k_max_tilt]              ; +max, d
        fcomi   st0, st1
        fcmovnb st0, st1                        ; min(max, d)
        fstp    st1
        fld     dword [k_max_tilt]
        fchs                                    ; -max, d
        fcomi   st0, st1
        fcmovb  st0, st1                        ; max(-max, d)
        fstp    st1
        fstp    dword [ebp-4]

        push    dword [ebp-4]
        push    dword STROKE_N
        push    buf_cand
        call    SYM(stroke_rotate)
        add     esp, 12

        push    dword STROKE_N
        push    buf_tmpl
        push    buf_cand
        call    SYM(stroke_score)
        add     esp, 12
        jmp     .out
.zero:
        fldz
.out:
        mov     esp, ebp
        pop     ebp
        ret

%ifidn __OUTPUT_FORMAT__, elf32
section .note.GNU-stack noalloc noexec nowrite progbits
%endif
