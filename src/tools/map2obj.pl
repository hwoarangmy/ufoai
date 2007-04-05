#!/bin/perl -w
#####################################################################
# map2obj.pl
#	radiant .map file parser and .obj (wavefront) converter
#	2007-04-05 Copyright Werner 'hoehrer' H�hrer bill_spam2 [AT) yahoo (DOT] de
#
# Licence:
#	The content of this file is under the GNU/GPL license v2.
#	http://www.gnu.org/licenses/gpl.txt
#
# Changelog:
#	2007-04-05	0.0.1	Basic parser	-- Werner 'hoehrer' H�hrer
#####################################################################
# usage: map2obj.pl <file.map>
#####################################################################

use strict;
use warnings;

my $version = "0.0.1";

############################
# Initialize the map data.
############################
sub map_init ($) {
	my ($map) = @_;
	
	# Init the material list
	$map->{materials} = {};
	$map->{materialcount} = 0;

	$map->{classes} = {};
	$map->{classcount} = 0;
	
	$map->{entities} = [];
	$map->{entitycount} = 0;
	
	# Init the brush-array
	$map->{brushes} = [];
	$map->{brushcount} = 0;
	
	# Init map filename (just for info)
	$map->{map_filename} = '';
	return $map;
}

sub brush_init ($) {
	my ($brush) = @_;

	$brush->{polygons} = [];
	$brush->{textures} = [];
	$brush->{polycount} = 0;	# Used to count polygons and textures (it's the same for both)
	
	$brush->{data} = {};
	return $brush;
}

sub entity_init ($) {
	my ($entity) = @_;

	# init entity
	$entity = {};
	$entity->{brushes} = [];
	$entity->{brushcount} = 0;
	$entity->{data} = {};
	return $entity;
}

sub str2coord ($) {
	my ($string) = @_;

	$string =~ s/^\s+//;
	my @coord = split(/\s+/, $string);
	
	return @coord;
}

sub obj_write($$) {
	my ($map, $filename) = @_;

	foreach my $entity  (@{$map->{entities}}) {
		if ((exists $entity->{data}->{classname}) && $entity->{data}->{classname} eq "worldspawn") {
			# TODO: Write brush data data.
		}

	}	
}

sub mtl_write($$) {
	my ($map, $filename) = @_;


	foreach my $mat  (keys %{$map->{materials}}) {
		# TODO: Write material data
	}
}

############################
# Parse the text from the map file.
############################
sub map_parse ($$) {
	my ($map, $filename) = @_;
	my $entity_open = 0;
	my $brush_open = 0;
	
	open(MAP_INPUT, "< ".$filename) ||
		die "Failed to open mapfile '", $filename, "' .\n";

	my $entity;
	my $brush;
		
	$map->{map_filename} = $filename;
	my $line;
	
	while(<MAP_INPUT>) {
		next if /^\s*\/\//;	# Skip comments
		next if /^$/;		# Skip newlines
		next if /^\s*$/;	# Skip line with only whitespaces

		chomp;	# Remove trailing whitespaces

		if (!$brush_open) {	# just in case
			$brush = {};
		}
		if (!$entity_open) {	# just in case
			$entity = {};
		}

		$line = $_;

		if ($line =~ m/^\s*{\s*$/) {
			# Opening curly bracket found.
			if ($entity_open) {
				if (!$brush_open) {
					# Brush opening curly-bracket found.
					
					# init brush (TODO: extra function?)
					$brush_open = 1;
					$brush = brush_init($brush);
				} else {
					print "Bad opening bracked found: '",$line,"'\n";
					print "Syntax of file possibly corrupted. Abort.\n";
					return;
				}
			} else {
				print "New enity.\n";
				# Entity opening curly-bracket found.
				$entity_open = 1;
				$entity = entity_init($entity);
		
				# Add brush to map-tree
				push (@{$map->{entities}}, $entity);
				$map->{entitycount}++;
			}
			next;
		} 
	
		if (($line =~ m/^\s*\(([^)]*)\)\s*\(([^)]*)\)\s*\(([^)]*)\)\s*([^\s]*)\s*([\d.\s]*)\s*$/) && $brush_open) {
			# Found a polygon with texture.
			#print "Parsing brush:", $map->{brushcount}-1," Polygon:", $brush->{polycount},"\n";
			my ($v1, $v2, $v3) = ($1, $2, $3);
			my $tex = $4;
			my $the_others = $5;

			$tex =~ s/^\s+//;
			my @vert1 = str2coord($v1);
			my @vert2 = str2coord($v2);
			my @vert3 = str2coord($v3);
			
			my $polygon = [[@vert1],[@vert2], [@vert3]];

			push (@{$brush->{polygons}}, $polygon);	# Store polygon.
			push (@{$brush->{textures}}, $tex);		# Store texture path.

			if (!exists($map->{materials}->{$tex})) {
				$map->{materials}->{$tex} = "mat".$map->{materialcount};		# Store info about all used textures (info only).
				$map->{materialcount}++;
			}

			$brush->{polycount}++;
			next;
		}
		if ($line =~ m/^\s*"([^"]*)"\s*"([^"]*)"\s*$/) {
			if ($brush_open) {
				# Found brush data
				# example: "origin" "416 -620 76"
				# $1 should be: origin
				# $2 should be: 416 -620 76
				$brush->{data}->{$1} = $2;
				#TODO: parse special cases for later export (like position and rotation).
				next;
			}
			if ($entity_open) {
				# Found brush data
				# example: "origin" "416 -620 76"
				# $1 should be: origin
				# $2 should be: 416 -620 76
				$entity->{data}->{$1} = $2;
				#TODO: parse special cases for later export (like position and rotation).
				if ($1 eq  'classname') {
					# print "Entityclass: ",$entity->{data}->{$1}, "\n"; Debug
					if (!exists($map->{classes}->{$entity->{data}->{$1} })) {
						$map->{classes}->{$entity->{data}->{$1} } = 1;
						$map->{classcount}++;
					}
				} elsif  (($1 eq  'origin') || ($1 eq  'color') || ($1 eq  '_color') || ($1 eq  'angles')){ 
					my @threes = str2coord($2);
					$entity->{data}->{$1} = @threes;
					
				}
				next;
			}
		}
	
		if ($line =~ m/^\s*}\s*$/) {
			# Closing curly bracket found
			if ($entity_open) {
				if ($brush_open) {
					# We are closing this brush
					$brush_open = 0;
					# Add brush to map-tree
					push (@{$map->{brushes}}, $brush);
					$map->{brushcount}++;
					push (@{$entity->{brushes}}, $brush);
					$entity->{brushcount}++;
				} else {
					# We are closing this enity.
					$entity_open = 0;
				}
			} else {
				print "Badclosing bracked found, we haven't even met an opening one: '",$line,"'\n";
				print "Syntax of file possibly corrupted. Abort.\n";
				return;
			}
			next;
		} 
		
	}
} # parse

########################
# MAIN
########################
my $map_filename = 'condor04.map'; # Dummy, never used
my $obj_filename;
my $map = {};
	
# parse commandline paarameters (md2-filenames)
if ( $#ARGV < 0 ) {
	die "Usage:\tmap2obj.pl <file.map>\n";
} elsif ( $#ARGV == 0 ) {
	$map_filename = $ARGV[0];
	print "Mapfile= \"". $map_filename, "\"\n";
}

# Generate output filename
if ($map_filename =~ m/.*\.map$/) {
	($obj_filename = $map_filename) =~ s/\.map$/\.obj/;
} else {
	$obj_filename = $map_filename.".obj";
}

$map = map_init($map);

# Open + parse map file
map_parse($map, $map_filename);

#Debug
#use Data::Dumper;
#print Dumper($map);

print "materials:\n";
foreach my $mat  (keys %{$map->{materials}}) {
	print " ",$mat," \t generated name:", $map->{materials}->{$mat},"\n";
}

print "classes:\n";
foreach my $class  (keys %{$map->{classes}}) {
	print "- ",$class,"\n";
}

# TODO: write obj (only types of "classname" that are supported by obj)
# TODO: write mtl