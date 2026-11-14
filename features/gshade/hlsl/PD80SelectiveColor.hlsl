// Generated from PD80_04_Selective_Color.fx by tools/gen_gshade_shaders.py
// GShade looks: ReShade -> ps_3_0 compatibility layer. Prepended to every ported shader.
// Only declarations live here; the shaders' maths stay as written in the GShade .fx files.
#define GSHADE_DITHER 0 // GShade.ini UseDithering=0
#define __RENDERER__ 0x9000 // D3D9, as GShade sees TS3: shaders take their D3D9 branches
#define __RESHADE__ 60703   // GShade 5.2.4 is based on ReShade 6.7.3
float4 gs_buf : register(c0);       // 1/W, 1/H, W, H of the back buffer
float4 gs_aux : register(c1);       // pass-specific
float4 gs_depth : register(c2);     // x = far plane (ReShade RESHADE_DEPTH_LINEARIZATION_FAR_PLANE), y = reversed
#define BUFFER_RCP_WIDTH   gs_buf.x
#define BUFFER_RCP_HEIGHT  gs_buf.y
#define BUFFER_WIDTH       gs_buf.z
#define BUFFER_HEIGHT      gs_buf.w
#define BUFFER_PIXEL_SIZE  gs_buf.xy
#define BUFFER_SCREEN_SIZE gs_buf.zw
#define BUFFER_COLOR_BIT_DEPTH 8
#define rcp(x) (1.0 / (x))
// ReShade's D3D9 code generator emulates texel offsets this way (ps_3_0 has no offset overload)
#define tex2Doffset(s, c, o) tex2D(s, (c) + (o) * BUFFER_PIXEL_SIZE)
#define tex2Dlodoffset(s, c, o) tex2Dlod(s, (c) + float4((o) * BUFFER_PIXEL_SIZE, 0.0, 0.0))
sampler2D BackBuffer : register(s0);
/*
    Description : PD80 04 Selective Color for Reshade https://reshade.me/
    Author      : prod80 (Bas Veth)
    License     : MIT, Copyright (c) 2020 prod80
    
    Additional credits
    - Based on the mathematical analysis provided here
      http://blog.pkh.me/p/22-understanding-selective-coloring-in-adobe-photoshop.html

    MIT License
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:
    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.
    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/


// ---- inlined PD80_00_Base_Effects.fxh ----
/*
 *  Collection of simple base effects often used across shaders by prod80
 *  Required for proper effect execution
 *  Includes [range]: exposure [-4, 4], contrast [-1, 1.5], brightness [-1, 1.5], saturation [-1, 1], vibrance[-1, 1]

 *  MIT License

 *  Copyright (c) 2020 prod80

 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:

 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.

 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

float3 sl( float3 c, float3 b )
{ 
    return b < 0.5f ? ( 2.0f * c * b + c * c * ( 1.0f - 2.0f * b )) :
                      ( sqrt( c ) * ( 2.0f * b - 1.0f ) + 2.0f * c * ( 1.0f - b ));
}

float getLum( in float3 x )
{
    return dot( x, float3( 0.212656, 0.715158, 0.072186 ));
}

float3 exposure( float3 res, float x )
{
    x = x < 0.0f ? x * 0.333f : x;
    return saturate( res.xyz * ( x * ( 1.0f - res.xyz ) + 1.0f ));
}

float3 con( float3 res, float x )
{
    //softlight
    float3 c = sl( res.xyz, res.xyz );
    x = ( x < 0.0f ) ? x * 0.5f : x;
    return saturate( lerp( res.xyz, c.xyz, x ));
}

float3 bri( float3 res, float x )
{
    //screen
    float3 c = 1.0f - ( 1.0f - res.xyz ) * ( 1.0f - res.xyz );
    x = ( x < 0.0f ) ? x * 0.5f : x;
    return saturate( lerp( res.xyz, c.xyz, x ));   
}

float3 sat( float3 res, float x )
{
    return saturate( lerp( getLum( res.xyz ), res.xyz, x + 1.0f ));
}

float3 vib( float3 res, float x )
{
    float4 sat = 0.0f;
    sat.xy = float2( min( min( res.x, res.y ), res.z ), max( max( res.x, res.y ), res.z ));
    sat.z = sat.y - sat.x;
    sat.w = getLum( res.xyz );
    return saturate( lerp( sat.w, res.xyz, 1.0f + ( x * ( 1.0f - sat.z ))));
}


namespace pd80_selectivecolor
{

    //// UI ELEMENTS ////////////////////////////////////////////////////////////////
    float corr_method : register(c8);
    float corr_method2 : register(c9);
    // Reds
    float r_adj_cya : register(c10);
    float r_adj_mag : register(c11);
    float r_adj_yel : register(c12);
    float r_adj_bla : register(c13);
    float r_adj_sat : register(c14);
    float r_adj_vib : register(c15);

    // Yellows
    float y_adj_cya : register(c16);
    float y_adj_mag : register(c17);
    float y_adj_yel : register(c18);
    float y_adj_bla : register(c19);
    float y_adj_sat : register(c20);
    float y_adj_vib : register(c21);
    
    // Greens
    float g_adj_cya : register(c22);
    float g_adj_mag : register(c23);
    float g_adj_yel : register(c24);
    float g_adj_bla : register(c25);
    float g_adj_sat : register(c26);
    float g_adj_vib : register(c27);

    // Cyans
    float c_adj_cya : register(c28);
    float c_adj_mag : register(c29);
    float c_adj_yel : register(c30);
    float c_adj_bla : register(c31);
    float c_adj_sat : register(c32);
    float c_adj_vib : register(c33);

    // Blues
    float b_adj_cya : register(c34);
    float b_adj_mag : register(c35);
    float b_adj_yel : register(c36);
    float b_adj_bla : register(c37);
    float b_adj_sat : register(c38);
    float b_adj_vib : register(c39);

    // Magentas
    float m_adj_cya : register(c40);
    float m_adj_mag : register(c41);
    float m_adj_yel : register(c42);
    float m_adj_bla : register(c43);
    float m_adj_sat : register(c44);
    float m_adj_vib : register(c45);

    // Whites
    float w_adj_cya : register(c46);
    float w_adj_mag : register(c47);
    float w_adj_yel : register(c48);
    float w_adj_bla : register(c49);
    float w_adj_sat : register(c50);
    float w_adj_vib : register(c51);

    // Neutrals
    float n_adj_cya : register(c52);
    float n_adj_mag : register(c53);
    float n_adj_yel : register(c54);
    float n_adj_bla : register(c55);
    float n_adj_sat : register(c56);
    float n_adj_vib : register(c57);

    // Blacks
    float bk_adj_cya : register(c58);
    float bk_adj_mag : register(c59);
    float bk_adj_yel : register(c60);
    float bk_adj_bla : register(c61);
    float bk_adj_sat : register(c62);
    float bk_adj_vib : register(c63);

    //// TEXTURES ///////////////////////////////////////////////////////////////////
    
    //// SAMPLERS ///////////////////////////////////////////////////////////////////

    //// DEFINES ////////////////////////////////////////////////////////////////////

    //// FUNCTIONS //////////////////////////////////////////////////////////////////
    
    float mid( float3 c )
    {
        float sum = c.x + c.y + c.z;
        float mn = min( min( c.x, c.y ), c.z );
        float mx = max( max( c.x, c.y ), c.z );
        return sum - mn - mx;
    }

    float adjustcolor( float scale, float colorvalue, float adjust, float bk, int method )
    {
        /* 
        y(value, adjustment) = clamp((( -1 - adjustment ) * bk - adjustment ) * method, -value, 1 - value ) * scale
        absolute: method = 1.0f - colorvalue * 0
        relative: method = 1.0f - colorvalue * 1
        */
        return clamp((( -1.0f - adjust ) * bk - adjust ) * ( 1.0f - colorvalue * method ), -colorvalue, 1.0f - colorvalue) * scale;
    }

    //// PIXEL SHADERS //////////////////////////////////////////////////////////////
    float4 PS_SelectiveColor(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
    {
        float4 color      = tex2D( BackBuffer, texcoord );

        // Clamp 0..1
        color.xyz         = saturate( color.xyz );

        // Need these a lot
        float min_value   = min( min( color.x, color.y ), color.z );
        float max_value   = max( max( color.x, color.y ), color.z );
        float mid_value   = mid( color.xyz );
        
        // Used for determining which pixels to adjust regardless of prior changes to color
        float3 orig       = color.xyz;

        // Scales
        float sRGB        = max_value - mid_value;
        float sCMY        = mid_value - min_value;
        float sNeutrals   = 1.0f - ( abs( max_value - 0.5f ) + abs( min_value - 0.5f ));
        float sWhites     = ( min_value - 0.5f ) * 2.0f;
        float sBlacks     = ( 0.5f - max_value ) * 2.0f;

        /*
        Create relative saturation levels.
        For example when saturating red channel you will manipulate yellow and magenta channels.
        So, to ensure there are no bugs and transitions are smooth, need to scale saturation with
        relative saturation of nearest colors. If difference between red and green is low ( color nearly yellow )
        you use this info to scale back red saturation on those pixels.
        This solution is not fool proof, but gives acceptable results almost always.
        */
        
        // Red is when maxvalue = x
        float r_d_m       = orig.x - orig.z;
        float r_d_y       = orig.x - orig.y;
        // Yellow is when minvalue = z
        float y_d         = mid_value - orig.z;
        // Green is when maxvalue = y
        float g_d_y       = orig.y - orig.x;
        float g_d_c       = orig.y - orig.z;
        // Cyan is when minvalue = x
        float c_d         = mid_value - orig.x;
        // Blue is when maxvalue = z
        float b_d_c       = orig.z - orig.y;
        float b_d_m       = orig.z - orig.x;
        // Magenta is when minvalue = y
        float m_d         = mid_value - orig.y;
        
        float r_delta     = 1.0f;
        float y_delta     = 1.0f;
        float g_delta     = 1.0f;
        float c_delta     = 1.0f;
        float b_delta     = 1.0f;
        float m_delta     = 1.0f;

        if( corr_method2 ) // Relative saturation
        {
            r_delta       = min( r_d_m, r_d_y );
            y_delta       = y_d;
            g_delta       = min( g_d_y, g_d_c );
            c_delta       = c_d;
            b_delta       = min( b_d_c, b_d_m );
            m_delta       = m_d;
        } 

        // Selective Color
        if( max_value == orig.x )
        {
            color.x       = color.x + adjustcolor( sRGB, color.x, r_adj_cya, r_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sRGB, color.y, r_adj_mag, r_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sRGB, color.z, r_adj_yel, r_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, r_adj_sat * r_delta );
            color.xyz     = vib( color.xyz, r_adj_vib * r_delta );
        }

        if( min_value == orig.z )
        {
            color.x       = color.x + adjustcolor( sCMY, color.x, y_adj_cya, y_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sCMY, color.y, y_adj_mag, y_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sCMY, color.z, y_adj_yel, y_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, y_adj_sat * y_delta );
            color.xyz     = vib( color.xyz, y_adj_vib * y_delta );
        }

        if( max_value == orig.y )
        {
            color.x       = color.x + adjustcolor( sRGB, color.x, g_adj_cya, g_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sRGB, color.y, g_adj_mag, g_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sRGB, color.z, g_adj_yel, g_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, g_adj_sat * g_delta );
            color.xyz     = vib( color.xyz, g_adj_vib * g_delta );
        }

        if( min_value == orig.x )
        {
            color.x       = color.x + adjustcolor( sCMY, color.x, c_adj_cya, c_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sCMY, color.y, c_adj_mag, c_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sCMY, color.z, c_adj_yel, c_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, c_adj_sat * c_delta );
            color.xyz     = vib( color.xyz, c_adj_vib * c_delta );
        }

        if( max_value == orig.z )
        {
            color.x       = color.x + adjustcolor( sRGB, color.x, b_adj_cya, b_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sRGB, color.y, b_adj_mag, b_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sRGB, color.z, b_adj_yel, b_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, b_adj_sat * b_delta );
            color.xyz     = vib( color.xyz, b_adj_vib * b_delta );
        }

        if( min_value == orig.y )
        {
            color.x       = color.x + adjustcolor( sCMY, color.x, m_adj_cya, m_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sCMY, color.y, m_adj_mag, m_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sCMY, color.z, m_adj_yel, m_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, m_adj_sat * m_delta );
            color.xyz     = vib( color.xyz, m_adj_vib * m_delta );
        }

        if( min_value >= 0.5f )
        {
            color.x       = color.x + adjustcolor( sWhites, color.x, w_adj_cya, w_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sWhites, color.y, w_adj_mag, w_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sWhites, color.z, w_adj_yel, w_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, w_adj_sat * smoothstep( 0.5f, 1.0f, min_value ));
            color.xyz     = vib( color.xyz, w_adj_vib * smoothstep( 0.5f, 1.0f, min_value ));
        }

        if( max_value > 0.0f && min_value < 1.0f )
        {
            color.x       = color.x + adjustcolor( sNeutrals, color.x, n_adj_cya, n_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sNeutrals, color.y, n_adj_mag, n_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sNeutrals, color.z, n_adj_yel, n_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, n_adj_sat );
            color.xyz     = vib( color.xyz, n_adj_vib );
        }

        if( max_value < 0.5f )
        {
            color.x       = color.x + adjustcolor( sBlacks, color.x, bk_adj_cya, bk_adj_bla, corr_method );
            color.y       = color.y + adjustcolor( sBlacks, color.y, bk_adj_mag, bk_adj_bla, corr_method );
            color.z       = color.z + adjustcolor( sBlacks, color.z, bk_adj_yel, bk_adj_bla, corr_method );
            color.xyz     = sat( color.xyz, bk_adj_sat * smoothstep( 0.5f, 0.0f, max_value ));
            color.xyz     = vib( color.xyz, bk_adj_vib * smoothstep( 0.5f, 0.0f, max_value ));
        }

        return float4( color.xyz, 1.0f );
    }

    //// TECHNIQUES /////////////////////////////////////////////////////////////////
    
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_pd80_selectivecolor_PS_SelectiveColor(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float4 c = pd80_selectivecolor::PS_SelectiveColor(float4(vp + 0.5, 0.0, 1.0), uv); return float4(c.rgb, 1.0); }
