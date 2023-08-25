# picovation
Controlling a Novation Circuit groovebox from a raspberry pico-based foot pedal.

The problem:

If you are a guitar, bass, drum etc player and you use a Novation Circuit groovebox to provide rhythm and synths while playing, you cannot use your hands to operate the groovebox.


The solution:

picovation is a foot pedal connected to the Novation Circuit groovebox allowing to control it with your foot instead of your hand. Picovanion is made of a Raspberry PI Pico and foot switches, and communicates as a MIDI USB host to groovebox. 

For this to work, your groovebox shall accept midi-clock IN information; it works well on my Novation Circuit for instance.
