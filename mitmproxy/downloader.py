#!/usr/bin/env python
# -*- coding: utf-8 -*-

import requests
import redis
import time
import hashlib
from common.Database import Database
import os



class DouyinDownloader(object):
    """docstring for DouyinDownloader"""
    def __init__(self, download_dir = '/tmp/download/'):
        super(DouyinDownloader, self).__init__()
        # self.arg = arg

        self.downloadCount = 0

        self.downloadDir = download_dir

        # redis connect
        self.redisClient = redis.StrictRedis(host='115.159.157.98', port=17379, db=0, password='redis12346')

        # mysql
        self.mysqlDB = Database(host='localhost', user='root', passwd='zx#Video2018', database='video')
        
    def downloadAll(self):
        while True:
            task = self.getTask()
            if task != None:
                print("get one task, to download")
                print(task)

                # 判断是否已经存在
                exist = self.videoExist(task)
                if exist == True:
                    print("video is exist, return")
                else:
                    self.downloadOne(task)

            time.sleep(1)


    def getTask(self):
        # 从任务队列中取任务
        task = self.redisClient.rpop("douyinTask")
        return task

    def downloadOne(self, task):

        nowtime = time.time()
        filename = str(nowtime) + ".mp4"
        dirname = time.strftime("%Y-%m-%d", time.localtime()) 
        filepath = self.downloadDir + dirname + '/' + filename

        print("to download video:" + filepath)

        # prepare dir
        os.makedirs(self.downloadDir + dirname, 666)

        # stream=True作用是推迟下载响应体直到访问Response.content属性
        res = requests.get(task, stream=True)
        # 将视频写入文件夹

        with open(filepath, 'ab') as f:
            f.write(res.content)
            f.flush()

            self.downloadCount = self.downloadCount + 1
            print(filename + '下载完成' + ', count:' + str(self.downloadCount))


            # 计算文件md5
            file_md5 = self.get_file_md5(filepath)
            print("file md5:" + file_md5)


            # 使用md5值作为新文件名
            new_file_path = self.downloadDir + file_md5 + ".mp4"


            videoInfo = self.mysqlDB.get_video_info_by_md5(file_md5)
            if videoInfo != None:
                # 文件已经存在
                print("video same md5 is exist, return")
            else:
                # 重命名
                os.rename(filepath, new_file_path)

                # 信息插入数据库
                urlmd5 = hashlib.md5(task).hexdigest()
                result,msg = self.mysqlDB.insert_video_info(platform=1, status=0, title='', url=str(task, encoding = "utf-8")  , md5=file_md5, urlmd5=urlmd5, storepath=new_file_path)
                print("insert_video_info:" + result)

    def videoExist(self, task):
        # 计算url的md5值，之后根据该值在数据库中查询， 从而判断该视频是否已经存在
        urlmd5 = hashlib.md5(task).hexdigest()

        # 根据urlMd5 查询视频信息
        videoInfo = self.mysqlDB.get_video_info(urlmd5)

        if videoInfo != None:
            return True

        return False

    def get_file_md5(self, file_path):
        f = open(file_path,'rb')  
        md5_obj = hashlib.md5()
        while True:
            d = f.read(8096)
            if not d:
                break
            md5_obj.update(d)
        hash_code = md5_obj.hexdigest()
        f.close()
        md5 = str(hash_code).lower()
        return md5


if __name__ == '__main__':
    douyin = DouyinDownloader('/root/douyin/')
    douyin.downloadAll()
