mkdir ..\ncs
cd ..\ncs
west init -m https://gitlab.com/apricitypublic/fw-nrfconnect-nrf.git
west update
cd nrf
git checkout Gateway
west update
