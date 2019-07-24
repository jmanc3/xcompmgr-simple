# xcompmgr-simple
An extremely small and fast xorg windows compositor that adds transparency to your desktop environment

## Purpose
As of the time of writing this, there exists no adequate project that serves the purpose of being an example of doing very basic windows compositing correctly. The closest thing we have is xcompmgr, but the code has many problems if it wants to be that. For instace,
* "c-isms"
    * meaningless and way too short variable names
    * terrible formatting
    * variables being declared at the top of the function instead of when used
* does much more than it should
    * shadows
    * fading on window closing
* and generally a high signal to noise ratio which makes it hard to understand

So I used xcompmgr as a base and ripped out all the code that was not essential to drawing windows with transparency and ended it up with 800~ lines of c++ code, while xcompmgr is 2000~ lines of badly written c code

I hope this project will help you in your ventures to write a windows compositor or even a windows manager that has a compositor (which is the reason I needed this to exist)

## Dependencies
* cmake
* a c++ compiler
* xorg development headers
* xorg extension headers

## Building with cmake
At the root of the project
```
mkdir out
cd out
cmake ../
make 
```
