cd engine

git fetch

git checkout <issue-branch>

git add .

git commit -m "feat: <files>. Closes #<issue-num>"

git push origin <issue-branch>

git checkout main

git pull origin main

git merge <issue-branch>

git push origin main

git branch -d <issue-branch>