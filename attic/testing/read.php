#!/usr/bin/php
uint16_t randstuff[] = {
<?php

$first = 1;
while (($c = fgetc(STDIN)) !== FALSE) {
	if ($c < '0' || $c > 'f') continue;
	if ($first) $first = 0;
	else echo(', ');
	echo(hexdec($c));
}
?>
};
