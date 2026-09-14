#version 330 core

out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D uDirect;      // full-res forward color
uniform sampler2D uSSR;         // half-res SSR result (blurred)
uniform sampler2D uAdditive;    // additive sprites to composite into final output
uniform sampler2D uAlpha;       // alpha sprites to composite into final output (including first person weapons)
uniform sampler2D uUI;          // the user interface to composite over the final BSP colors

void main()
{
    vec3 direct   = texture(uDirect,    TexCoords).rgb;
    vec4 ssrTex   = texture(uSSR,       TexCoords);
    vec4 additive = texture(uAdditive,  TexCoords);
    vec4 alpha    = texture(uAlpha,     TexCoords);
    vec4 ui       = texture(uUI,        TexCoords);
    
    vec3 ssr = ssrTex.rgb;
    // SSR alpha is stored in uSSR's alpha channel (raymarch hit strength)
    float ssrStrength = ssrTex.a;

    // First: world + reflections
    vec3 color = mix(direct, ssr, ssrStrength);

    // sprites + UI is drawn OVER the world+SSR
    color = additive.rgb + color * (1.0 - additive.rgb); // additive blending
    color = mix(color, alpha.rgb, alpha.a); // alpha blend
    color = ui.rgb + color * (1.0 - ui.rgb); // additive blending

    FragColor = vec4(color, 1.0);
}
