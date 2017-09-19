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
line4 += "};"

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
        if i < Total_Node:
            print line5 + ","
        else:
            print line5 + " };" 

print "int CONF_allPort[] = {",
for i in range(1, Total_Node+1):
    if i != Node_ID:
        if Node_ID < i:
            line6 = "56" + str(Node_ID) + str(i)
        else:
            line6 = "56" + str(i) + str(Node_ID)
        if i < Total_Node:
            print line6 + ",",
        else:
            print line6 + " };"

