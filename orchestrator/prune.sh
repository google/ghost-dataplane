# A script to stop and remove all containers.
echo "Stopping all containers"
docker stop $(docker ps -a -q)
echo "Removing all containers"
docker rm $(docker ps -a -q)
