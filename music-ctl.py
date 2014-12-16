#!/usr/bin/python

password = "woeiFImkweFOWKOkfweNblkweE"
host = "18.109.6.97"
homedir = "/home/sixtwoseventy" #for storing last round number info
maxtrack = 9


import commands,sys,os

controls = dict()

mpc = "mpc -h " + password + "@" + host + " "

controls['toggle'] = mpc+'toggle'
controls['stop'] = mpc+'stop'
controls['play'] = mpc+'play'
controls['clear'] = mpc+'clear'
controls['volume-rampdown'] = mpc+'volume -40'
controls['volume-max'] = mpc+'volume 100'
controls['volume-min'] = mpc+'volume 0'
controls['volume-off'] = mpc+'volume 0'
controls['add'] = mpc+'add'
controls['stop'] = mpc+'stop'
controls['stop'] = mpc+'stop'



#TODO volume controls didn't work on my laptop. So, test this bit beforehand

def shellExec(cmd):
    retval = commands.getstatusoutput(controls[cmd])
    if retval[0]:
        raise(Exception(retval[1]))
    return

def cleanSlate():
    shellExec("clear")
    #shellExec("volume-max")
    return

def addSong(songName):
    shellExec('')
    return

def playTrack(roundNum):
    commands.getstatusoutput(mpc+"add CUTS/"+str(roundNum)+".mp3")
    shellExec("play")
    
    writeRoundNum(roundNum)
    return

def playFirst():
    commands.getstatusoutput(mpc+"add CUTS/1.mp3")
    shellExec("play")
    writeRoundNum(1)
    return

def playNext():
    if readRoundNum()>maxtrack:
        return playFirst()
    return playTrack(readRoundNum()+1)

def printUsage():
    print "Usage: music-ctl.py ([round_number]|reset|shutup)"
    print "if round_number is omitted,"
    print "this script attempts to play music for the next"
    print "round, and keeps a counter for which round it is on"
    return

def readRoundNum():
    if os.path.exists("/tmp/roundstate.txt"):
        return int(open("/tmp/roundstate.txt").read())
    else:
        return 1
    
def writeRoundNum(num):
    open("/tmp/roundstate.txt",'w').write(str(num))
    open(homedir+"/roundstate.txt",'w').write(str(num))
    
    
if __name__=="__main__":
    #running as shell script
    argc = len(sys.argv)-1
    argv = sys.argv[1:]
    if argc == 0:
        printUsage()
        print "Attempting to queue next track..."
        cleanSlate()
        playNext()
    elif argv[0] == "reset":
        cleanSlate()
        if os.path.exists("/tmp/roundstate.txt"):
            os.remove("/tmp/roundstate.txt")
    elif argv[0] == "shutup":
        cleanSlate()
    elif int(argv[0])>0:
        cleanSlate()
        playTrack(int(argv[0]))
    else:
        print "I didn't understand what you wanted."
