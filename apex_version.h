#pragma once
// Version of Apex Radiance for The Sims 3 (ApexRadiance.asi).
#define APEX_VERSION_MAJOR 2
#define APEX_VERSION_MINOR 11
#define APEX_VERSION_PATCH 1
#define APEX_STRINGIZE_INNER(x) #x
#define APEX_STRINGIZE(x) APEX_STRINGIZE_INNER(x)
#define APEX_VERSION_NUMBER APEX_STRINGIZE(APEX_VERSION_MAJOR) "." APEX_STRINGIZE(APEX_VERSION_MINOR) "." APEX_STRINGIZE(APEX_VERSION_PATCH)
// A stable release is the bare number; for a pre-release write e.g. APEX_VERSION_NUMBER "-beta"
#ifdef APEX_F10_STUDY
#define APEX_VERSION_STRING APEX_VERSION_NUMBER "-f10-study"
#else
#define APEX_VERSION_STRING APEX_VERSION_NUMBER "-performance-test"
#endif

// Visible product name (menu header, credits, feature descriptions, log). Internal identifiers keep "Apex"; files are
// ApexRadiance.asi, ApexRadiance.toml, ApexRadiance_LOG.txt in Documents\...\Apex Radiance\ (see framework/apex_paths.h).
#define APEX_PRODUCT_NAME "Apex Radiance"
#define APEX_PRODUCT_TAGLINE "for The Sims 3"
#define APEX_LOGO_LETTER "A" // the letter on the menu's logo tile
