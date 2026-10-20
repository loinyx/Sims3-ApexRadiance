#!/usr/bin/perl
# Writes i18n/keys.tsv: every English key of the four-column (EN/PT/ES/FR) tables (i18n/tr_*.cpp and the .inc files they include),
# once each, for the people and tools translating the menu into the languages of i18n/lang_<code>.cpp.
# Run from anywhere: perl tools/i18n_keys.pl   (tools/i18n_check reports when keys.tsv no longer matches the tables)
#
# keys.tsv: UTF-8, LF line ends, tab-separated, one header line, then one line per key in the order the menu
# registers them (texts of one screen stay together). Columns:
#   key           the English text, byte-identical to the key the code looks up (C escapes and APEX_* macros resolved)
#   table         where it is defined: file:array (several, comma-separated, when two tables share the key)
#   pt, es, fr    the existing Portuguese (Brazil), Spanish and French translations, as hints for meaning and length
#   placeholders  the {} format placeholders of the key, in order, space-separated (a translation keeps all of them)
# Inside a field, backslash, tab, line feed and carriage return are written as \\, \t, \n and \r.
use strict;
use warnings;
use File::Basename qw(dirname basename);
use File::Spec;
use File::Glob qw(bsd_glob);

my $root = File::Spec->rel2abs(File::Spec->catdir(dirname(__FILE__), '..'));
my $i18n = "$root/i18n";

# String macros the tables concatenate (APEX_PRODUCT_NAME, APEX_PRODUCT_TAGLINE, ...): #define NAME "literal"
my %macros;
open my $vh, '<:raw', "$root/apex_version.h" or die "apex_version.h: $!";
while (<$vh>) {
    $macros{$1} = Unescape($2) if /^\s*#\s*define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"\s*(?:\/\/.*)?$/;
}
close $vh;

# The tables in link order (ApexRadiance.vcxproj lists them in this order; the order only groups the output)
my @files = grep { -f "$i18n/$_" } qw(tr_widgets.cpp tr_menu.cpp tr_image.cpp tr_lighting.cpp tr_features.cpp);
for my $f (sort map { basename($_) } bsd_glob("$i18n/tr_*.cpp")) {
    push @files, $f unless grep { $_ eq $f } @files;
}

my (@order, %rows);
ParseFile($_) for @files;

my $out = "$i18n/keys.tsv";
open my $o, '>:raw', $out or die "$out: $!";
print $o join("\t", qw(key table pt es fr placeholders)), "\n";
for my $key (@order) {
    my $r = $rows{$key};
    print $o join("\t", map { Field($_) } $key, join(',', @{$r->{tables}}), @{$r->{tr}}, join(' ', Placeholders($key))), "\n";
}
close $o;
printf "%d keys written to %s\n", scalar(@order), $out;

# ---- C++ reading ----

sub ParseFile {
    my ($name) = @_;
    my @tokens = Tokenize($name);
    my $i = 0;
    while ($i < @tokens) {
        # I18n::Entry <array>[] = { {a, b, c, d}, ... };
        if ($tokens[$i][0] eq 'id' && $tokens[$i][1] eq 'Entry' && $i + 5 < @tokens && $tokens[$i + 1][0] eq 'id'
            && $tokens[$i + 2][1] eq '[' && $tokens[$i + 3][1] eq ']' && $tokens[$i + 4][1] eq '=' && $tokens[$i + 5][1] eq '{') {
            my $array = $tokens[$i + 1][1];
            $i += 6;
            while ($i < @tokens && $tokens[$i][1] ne '}') {
                die "$name:$array: expected '{' at token $i\n" unless $tokens[$i][1] eq '{';
                $i++;
                my @values = ('');
                while ($tokens[$i][1] ne '}') {
                    my $t = $tokens[$i];
                    if ($t->[1] eq ',') { push @values, ''; }
                    elsif ($t->[0] eq 'str') { $values[-1] .= $t->[1]; }
                    elsif ($t->[0] eq 'id' && $t->[1] eq 'nullptr') { }
                    elsif ($t->[0] eq 'id' && exists $macros{$t->[1]}) { $values[-1] .= $macros{$t->[1]}; }
                    else { die "$name:$array: unexpected '$t->[1]'\n"; }
                    $i++;
                }
                $i++;                                   # '}'
                $i++ if $i < @tokens && $tokens[$i][1] eq ',';
                die "$name:$array: an entry has " . scalar(@values) . " values instead of 4\n" unless @values == 4;
                my ($en, @tr) = @values;
                next if $en eq '';
                if (!$rows{$en}) {
                    $rows{$en} = { tables => [], tr => \@tr };
                    push @order, $en;
                }
                my $where = "$tokens[$i - 1][2]:$array";
                push @{$rows{$en}{tables}}, $where unless grep { $_ eq $where } @{$rows{$en}{tables}};
            }
        }
        $i++;
    }
}

# Tokens: [kind, text, file] with kind str (decoded bytes), id or punct. Comments are dropped; #include "x.inc" is
# expanded in place (tr_menu.cpp includes tr_developer.inc), other preprocessor lines are skipped.
sub Tokenize {
    my ($name) = @_;
    open my $f, '<:raw', "$i18n/$name" or die "$name: $!";
    local $/;
    my $src = <$f>;
    close $f;
    my @tokens;
    pos($src) = 0;
    while (pos($src) < length $src) {
        if ($src =~ /\G\s+/gc) { next; }
        if ($src =~ /\G\/\/[^\n]*/gc) { next; }
        if ($src =~ /\G\/\*.*?\*\//gcs) { next; }
        if ($src =~ /\G#\s*include\s*"([^"]+\.inc)"[^\n]*/gc) { push @tokens, Tokenize($1); next; }
        if ($src =~ /\G#[^\n]*/gc) { next; }
        if ($src =~ /\G(?:u8)?"((?:[^"\\\n]|\\.)*)"/gc) { push @tokens, ['str', Unescape($1), $name]; next; }
        if ($src =~ /\G'(?:[^'\\]|\\.)*'/gc) { push @tokens, ['punct', 'char', $name]; next; }
        if ($src =~ /\G([A-Za-z_]\w*)/gc) { push @tokens, ['id', $1, $name]; next; }
        if ($src =~ /\G(::)/gc) { push @tokens, ['punct', $1, $name]; next; }
        if ($src =~ /\G(.)/gcs) { push @tokens, ['punct', $1, $name]; next; }
    }
    return @tokens;
}

# C string-literal escapes to bytes (the sources are UTF-8, so plain characters stay as they are)
sub Unescape {
    my ($s) = @_;
    my %simple = (n => "\n", t => "\t", r => "\r", '0' => "\0", '\\' => '\\', '"' => '"', "'" => "'", a => "\a", b => "\b", f => "\f", v => "\x0B", '?' => '?');
    $s =~ s{\\(?:x([0-9A-Fa-f]+)|([0-7]{1,3})|(.))}{
        defined $1 ? chr(hex($1) & 0xFF) : defined $2 ? chr(oct($2) & 0xFF) : exists $simple{$3} ? $simple{$3} : die "unknown escape \\$3\n"
    }ge;
    return $s;
}

# Same rule as I18n's Placeholders(): {...} groups in order, skipping the escaped {{ and }}
sub Placeholders {
    my ($s) = @_;
    my @out;
    while ($s =~ /\G(?:\{\{|\}\}|(\{[^}]*\})|.)/gcs) { push @out, $1 if defined $1; }
    return @out;
}

sub Field {
    my ($s) = @_;
    $s = '' unless defined $s;
    $s =~ s/\\/\\\\/g;
    $s =~ s/\t/\\t/g;
    $s =~ s/\n/\\n/g;
    $s =~ s/\r/\\r/g;
    return $s;
}
