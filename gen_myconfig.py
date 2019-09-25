import sys

Total_Node = int(sys.argv[1]) + 1
Node_ID    = int(sys.argv[2])
IP_subnet  = "\"" + sys.argv[4]

line1 = "int CONF_totalCB = " + sys.argv[1] + ";"
line2 = "int CONF_nodeID  = " + sys.argv[2] + ";"
line3 = "int CONF_localpagecount = " + sys.argv[3] + ";"
line4 = "int CONF_isServer[] = { "

for i in range(1, Total_Node+1):
    if i != Node_ID:
        if Node_ID < i:
            line4 += "1"
        else:
            line4 += "0"
        if i < Total_Node:
            line4 += ", "
if Node_ID == Total_Node:
    line4 = line4[:-2]
line4 += " };"

print line1
print line2
print line3
print line4

print "char *CONF_allIP[] = {",
for i in range(1, Total_Node+1):
    if i != Node_ID:
        if Node_ID < i:
            line5 = IP_subnet + str(Node_ID) + "\""
        else:
            line5 = IP_subnet + str(i) + "\""

        if Node_ID == Total_Node and i == (Total_Node-1):
            print line5 + " };" 
        elif i < Total_Node:
            print line5 + ","
        else:
            print line5 + " };" 

print "int CONF_allPort[] = {",
for i in range(1, Total_Node+1):
    if i != Node_ID:
        if Node_ID<10 and i<10:
            if Node_ID < i:
                line6 = "52" + str(Node_ID) + str(i)
            else:
                line6 = "52" + str(i) + str(Node_ID)
        else:
            if Node_ID < i:
                line6 = "4" + str(Node_ID) + str(i)
            else:
                line6 = "4" + str(i) + str(Node_ID)

        if Node_ID == Total_Node and i == (Total_Node-1):
            print line6 + " };"
        elif i < Total_Node:
            print line6 + ",",
        else:
            print line6 + " };"

