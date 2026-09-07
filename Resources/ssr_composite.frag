#version 330 core

out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D uDirect;      // full-res forward color
uniform sampler2D uSSR;         // half-res SSR result (blurred)
uniform sampler2D uSSRBuffer;   // full-res SSR buffer (depth + roughness + oct normal)
uniform sampler2D uUI;          // the user interface and other billboards to composite over the final BSP colors

void main()
{
    vec3 direct  = texture(uDirect, TexCoords).rgb;
    vec4 ssrTex  = texture(uSSR,    TexCoords).rgba;
    vec4 ui      = texture(uUI, TexCoords);
    
    vec3 ssr = ssrTex.rgb;
    // SSR alpha is stored in uSSR's alpha channel (raymarch hit strength)
    float ssrStrength = ssrTex.a;

    // First: world + reflections
    vec3 color = mix(direct, ssr, ssrStrength);

    // UI is drawn OVER the world+SSR
    //color = mix(color, ui.rgb, ui.a); // alpha blend
    color = ui.rgb + color * (1.0 - ui.rgb);


    FragColor = vec4(color, 1.0);
}
