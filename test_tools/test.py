import pyvisa
rm = pyvisa.ResourceManager()
inst = rm.open_resource("TCPIP0::192.168.1.176::inst0::INSTR")
inst.timeout = 5000
print(inst.query("*IDN?"))