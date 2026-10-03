#!/usr/bin/perl
# Apex Radiance menu audit (read-only). Usage: perl audit.pl [repo]
# Reports: menu texts without a translation entry, copy-guideline breaks (docs/ui.md "Copy guidelines"), settings keys
# missing from docs/, and settings not put back by a ResetDefaults() in the same file.
use strict; use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use File::Find qw(find);
use File::Spec;

my $repo_arg = shift;
my $repo = $repo_arg // File::Spec->catdir(dirname(__FILE__), '..', '..', '..');
$repo = abs_path($repo) or die "no repo $repo\n";
chdir $repo or die "cannot enter repo $repo\n";
sub slurp { my $f = shift; open my $h, '<:raw', $f or return ''; local $/; my $d = <$h>; $d =~ s/\r//g; return $d; }
my @src;
find({ wanted => sub {
    return unless -f $_ && /\.(?:cpp|h)\z/;
    my $path = $File::Find::name;
    return if $path =~ m{\A\./(?:i18n|tools|\.claude|\.codex|Release|Public|vcpkg|x64|packages)/};
    push @src, $path;
}, no_chdir => 0 }, '.');
my $lit = qr/"((?:[^"\\]|\\.)*)"/;

# 1. translation keys: the first element of every {"English", ...} entry (adjacent literals joined)
my %tr;
for my $f (glob 'i18n/tr_*.cpp') {
    my $d = slurp($f);
    while ($d =~ /\{\s*((?:"(?:[^"\\]|\\.)*"\s*(?:APEX_PRODUCT_NAME\s*)?)+),/g) {
        my $k = $1; my $s = '';
        while ($k =~ /$lit|(APEX_PRODUCT_NAME)/g) { $s .= defined $1 ? $1 : 'Apex Radiance' }
        $tr{$s} = 1;
    }
}

# 2. menu texts drawn by the ApexUi widgets: which argument positions carry text
my %calls = (SwitchRow => [0, 2], SliderPercent => [0, 4], SegmentedRow => [0, 1], GroupLabel => [0], TextButton => [0, 1],
             IconTextButton => [0, 2], CardHeader => [1, 2], PageTitle => [0, 1], SetNextRowBadge => [0, 1], IconNote => [1],
             Slider => [0], ReportChange => [0]);
my (@missing, @copy, %seen);
sub args {
    my $s = shift; my @a; my ($depth, $cur, $inq) = (0, '', 0);
    for my $i (0 .. length($s) - 1) {
        my $ch = substr($s, $i, 1);
        if ($inq) { $cur .= $ch; $inq = 0 if $ch eq '"' && substr($s, $i - 1, 1) ne '\\'; next }
        if ($ch eq '"') { $inq = 1; $cur .= $ch; next }
        if ($ch eq ',' && !$depth) { push @a, $cur; $cur = ''; next }
        $depth++ if $ch =~ /[(\[{]/; $depth-- if $ch =~ /[)\]}]/;
        $cur .= $ch;
    }
    push @a, $cur; return @a;
}
for my $f (@src) {
    next if $f =~ m{ui/(widgets|i18n)\.cpp$};
    my $d = slurp($f);
    for my $fn (keys %calls) {
        while ($d =~ /ApexUi::$fn\(((?:[^()"]|"(?:[^"\\]|\\.)*"|\((?:[^()"]|"(?:[^"\\]|\\.)*")*\))*)\)/g) {
            my @a = args($1);
            for my $i (@{$calls{$fn}}) {
                next unless defined $a[$i] && $a[$i] =~ /^\s*$lit\s*$/;
                my $key = $1; $key =~ s/##.*//;
                next if $key eq '' || $seen{"$f|$fn|$key"}++;
                my $isLabel = ($i == 0 && $fn =~ /^(SwitchRow|SliderPercent|SegmentedRow|Slider|TextButton|IconTextButton)$/) || ($fn eq 'CardHeader' && $i == 1);
                push @missing, "$f: $fn \"$key\"" unless $tr{$key};
                push @copy, "$f: label over 32 chars: \"$key\"" if $isLabel && length($key) > 32;
                push @copy, "$f: text over 90 chars: \"$key\"" if !$isLabel && length($key) > 90;
                push @copy, "$f: ends with a period: \"$key\"" if $key =~ /[^.]\.$/ && $key !~ /Credits: \@loinyx$/;
            }
        }
    }
    while ($d =~ /\bo\.(tooltip|leftLabel|rightLabel)\s*=\s*$lit/g) {
        my ($what, $t) = ($1, $2); next if $seen{"$f|o|$t"}++;
        push @missing, "$f: SliderOptions.$what \"$t\"" unless $tr{$t};
    }
    while ($d =~ /kTabs\[\]\s*=\s*\{([^}]*)\}/g) {
        my $l = $1; while ($l =~ /$lit/g) { push @missing, "$f: tab \"$1\"" unless $tr{$1} || $seen{"$f|tab|$1"}++ }
    }
}

# 3. settings: documented in docs/? put back by ResetDefaults()?
my (@undoc, @noreset);
my @docs;
find({ wanted => sub { push @docs, $File::Find::name if -f $_ && /\.md\z/ }, no_chdir => 0 }, 'docs');
my $docs = join "\n", map { slurp($_) } @docs;
for my $f (@src) {
    my $d = slurp($f);
    my ($reset) = $d =~ /ResetDefaults\(\)\s*\{(.*?)\n    \}/s;
    while ($d =~ /Register\w*Setting\(\s*&(\w+)\s*,\s*"(\w+)"/g) {
        my ($var, $key) = ($1, $2);
        push @undoc, "$f: $key" unless $docs =~ /\b\Q$key\E\b/;
        push @noreset, "$f: $key ($var)" if defined $reset && $reset !~ /\b\Q$var\E\b/;
    }
}

sub section { my ($t, @l) = @_; printf "== %s: %d\n", $t, scalar @l; print "   $_\n" for sort @l; }
section('Menu texts without a translation entry (i18n/tr_*.cpp)', @missing);
section('Copy guideline breaks (docs/ui.md "Copy guidelines")', @copy);
section('Settings keys not in docs/ (add them to the feature doc settings table)', @undoc);
section('Settings not put back by ResetDefaults() in their file', @noreset);
print "(Texts built at run time, Trf formats and raw ImGui text are not seen here: check Developer > Language in game.)\n";
